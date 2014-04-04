/*
 * gpuscan.c
 *
 * Sequential scan accelerated by GPU processors
 * ----
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#include "postgres.h"
#include "node/relation.h"
#include "pg_strom.h"

static add_scan_path_hook_type	add_scan_path_next;
static CustomPathMethods		gpuscan_path_methods;
static CustomPlanMethods		gpuscan_plan_methods;
static bool						enable_gpuscan;

typedef struct {
	CustomPath	cpath;
	List	   *dev_quals;		/* RestrictInfo run on device */
	List	   *host_quals;		/* RestrictInfo run on host */
	Bitmapset  *dev_attrs;		/* attrs referenced in device */
	Bitmapset  *host_attrs;		/* attrs referenced in host */
} GpuScanPath;

typedef struct {
	CustomPlan	cplan;
	Index		scanrelid;		/* index of the range table */
	const char *kern_source;	/* source of opencl kernel */
	int			extra_flags;	/* extra libraries to be included */
	List	   *used_params;	/* list of Const/Param in use */
	List	   *used_vars;		/* list of Var in use */
	List	   *dev_clauses;	/* clauses to be run on device */
	Bitmapset  *dev_attrs;		/* attrs referenced in device */
	Bitmapset  *host_attrs;		/* attrs referenced in host */
} GpuScanPlan;

/*
 * Gpuscan has three strategy to scan a relation.
 * a) cache-only scan, if all the variables being referenced in target-list
 *    and scan-qualifiers are on the t-tree columnar cache.
 *    It is capable to return a columner-store, instead of individual rows,
 *    if upper plan node is also managed by PG-Strom.
 * b) hybrid-scan, if Var references by scan-qualifiers are on cache, but
 *    ones by target-list are not. It runs first screening by device, then
 *    fetch a tuple from the shared buffers.
 * c) heap-only scan, if all the variables in scan-qualifier are not on
 *    the cache, all we can do is read tuples from shared-buffer to the
 *    row-store, then picking it up.
 * In case of (a) and (b), gpuscan needs to be responsible to MVCC checks;
 * that is not done on the first evaluation timing.
 * In case of (c), it may construct a columnar cache entry that caches the
 * required columns.
 */
#define GpuScanMode_CacheOnlyScan	0x0001
#define GpuScanMode_HybridScan		0x0002
#define GpuScanMode_HeapOnlyScan	0x0003
#define GpuScanMode_CreateCache		0x0004


typedef struct {
	CustomPlanState		cps;
	Relation			scan_rel;
	HeapScanDesc		scan_desc;
	TupleTableSlot	   *scan_slot;
	int					scan_mode;
	shmem_context	   *shmcontext;
	pgstrom_queue	   *mqueue;
	pgstrom_parambuf   *parambuf;
	Datum				dprog_key;
	dlist_head			ready_chunks;
	dlist_head			free_chunks;
} GpuScanState;


static void
cost_gpuscan(GpuScanPath *gpu_path, PlannerInfo *root,
			 RelOptInfo *baserel, ParamPathInfo *param_info,
			 List *dev_quals, List *host_quals)
{
	Cost		startup_cost = 0;
	Cost		run_cost = 0;
	double		spc_seq_page_cost;
	QualCost	dev_cost;
	QualCost	host_cost;
	Cost		gpu_per_tuple;
	Cost		cpu_per_tuple;
	Selectivity	dev_sel;

	/* Should only be applied to base relations */
	Assert(baserel->relid > 0);
	Assert(baserel->rtekind == RTE_RELATION);

	/* Mark the path with the correct row estimate */
	if (param_info)
		path->rows = param_info->ppi_rows;
	else
		path->rows = baserel->rows;

	if (!enable_gpuscan)
		startup_cost += disable_cost;

	/* fetch estimated page cost for tablespace containing table */
	get_tablespace_page_costs(baserel->reltablespace,
							  NULL,
							  &spc_seq_page_cost);
	/*
	 * disk costs
	 * XXX - needs to adjust after columner cache in case of bare heapscan,
	 * or partial heapscan if targetlist references out of cached columns
	 */
    run_cost += spc_seq_page_cost * baserel->pages;

	/* GPU costs */
	cost_qual_eval(&dev_cost, dev_quals, root);
	dev_sel = clauselist_selectivity(root, dev_quals, 0, JOIN_INNER, NULL);

	/*
	 * XXX - very rough estimation towards GPU startup and device calculation
	 *       to be adjusted according to device info
	 *
	 * TODO: startup cost takes NITEMS_PER_CHUNK * width to be carried, but
	 * only first chunk because data transfer is concurrently done, if NOT
	 * integrated GPU
	 * TODO: per_tuple calculation cost shall be divided by parallelism of
	 * average opencl spec.
	 */
	dev_cost.startup += 10000;
	dev_cost.per_tuple /= 100;

	/* CPU costs */
	cost_qual_eval(&host_cost, host_quals, root);
	if (param_info)
	{
		QualCost	param_cost;

		/* Include costs of pushed-down clauses */
		cost_qual_eval(&param_cost, param_info->ppi_clauses, root);
		host_cost.startup += param_cost.startup;
		host_cost.per_tuple += param_cost.per_tuple;
	}

	/* total path cost */
	startup_cost += dev_cost.startup + host_cost.startup;
	cpu_per_tuple = cpu_tuple_cost + host_cost.per_tuple;
	gpu_per_tuple = cpu_tuple_cost / 100 + dev_cost.per_tuple;
	run_cost += (gpu_per_tuple * baserel->tuples +
				 cpu_per_tuple * dev_sel * baserel->tuples);

    gpu_path->cpath.path.startup_cost = startup_cost;
    gpu_path->cpath.path.total_cost = startup_cost + run_cost;
}

static void
gpuscan_add_scan_path(PlannerInfo *root,
					  RelOptInfo *baserel,
					  RangeTblEntry *rte)
{
	GpuScanPath	   *pathnode;
	List		   *dev_quals = NIL;
	List		   *host_quals = NIL;
	Bitmapset	   *dev_attrs = NULL;
	Bitmapset	   *host_attrs = NULL;
	bool			has_sysattr = false;
	ListCell	   *cell;
	codegen_context	context;

	/* check whether qualifier can run on GPU device */
	memset(&context, 0, sizeof(codegen_context));
	foreach (cell, baserel->baserestrictinfo)
	{
		RestrictInfo   *rinfo = lfirst(cell);

		if (pgstrom_codegen_available_expression(rinfo->clause))
		{
			pull_varattnos(rinfo->clause, &dev_attrs);
			dev_quals = lappend(dev_quals, rinfo);
		}
		else
		{
			pull_varattnos(rinfo->clause, &host_attrs);
			host_quals = lappend(host_quals, rinfo);
		}
	}
	/* also, picks up Var nodes in the target list */
	pull_varattnos(baserel->reltargetlist, &host_attrs);

	/*
	 * FIXME: needs to pay attention for projection cost.
	 * It may make sense to use build_physical_tlist, if host_attrs
	 * are much wider than dev_attrs.
	 * Anyway, it needs investigation of the actual behavior.
	 */

	/* XXX - check whether columnar cache may be available */

	/*
	 * Construction of a custom-plan node.
	 */
	pathnode = palloc0(GpuScanPath);
	pathnode->cpath.path.type = T_CustomPath;
	pathnode->cpath.path.pathtype = T_CustomPlan;
	pathnode->cpath.path.parent = baserel;
	pathnode->cpath.path.param_info
		= get_baserel_parampathinfo(root, rel, rel->lateral_relids);
	pathnode->cpath.path.pathkeys = NIL;	/* gpuscan has unsorted result */

	cost_gpuscan(pathnode, root, rel,
				 pathnode->cpath.path->param_info,
				 dev_quals, host_quals);

	pathnode->dev_quals = dev_quals;
	pathnode->host_quals = host_quals;
	pathnode->dev_attrs = dev_attrs;
	pathnode->host_attrs = host_attrs;

	add_paths(rel, &pathnode->cpath.path);
}

static char *
gpuscan_codegen_quals(PlannerInfo *root, List *dev_quals,
					  codegen_context *context)
{
	StringInfoData	str;
	ListCell	   *cell;
	int				index;
	devtype_info   *dtype;
	char		   *expr_code;

	memset(context, 0, sizeof(codegen_context));
	if (dev_quals == NIL)
		return NULL;

	expr_code = pgstrom_codegen_expression((Node *)dev_quals, context);

	Assert(expr_code != NULL);

	initStringInfo(&str);

	/* Put param/const definitions */
	index = 0;
	foreach (cell, context->used_params)
	{
		if (IsA(lfirst(cell), Const))
		{
			Const  *con = lfirst(cell);

			dtype = pgstrom_devtype_lookup(con->consttype);
			Assert(dtype != NULL);

			appendStringInfo(&str,
							 "#define KPARAM_%u\t"
							 "pg_%s_param(kparams,%d)\n",
							 index, dtype->type_ident, index);
		}
		else if (IsA(lfirst(cell), Param))
		{
			Param  *param = lfirst(cell);

			dtype = pgstrom_devtype_lookup(param->paramtype);
			Assert(dtype != NULL);

			appendStringInfo(&str,
							 "#define KPARAM_%u\t"
							 "pg_%s_param(kparams,%d)\n",
							 index, dtype->type_ident, index);
		}
		else
			elog(ERROR, "unexpected node: %s", nodeToString(lfirst(cell)));
		index++;
	}

	/* Put Var definition for row-store */
	index = 0;
	foreach (cell, context->used_vars)
	{
		Var	   *var = lfirst(cell);

		dtype = pgstrom_devtype_lookup(var->vartype);
		Assert(dtype != NULL);

		if (dtype->type_flags & DEVTYPE_IS_VARLENA)
			appendStringInfo(&str,
					 "#define KVAR_%u\t"
							 "pg_%s_vref(kcs,toast,%u,get_global_id(0))\n",
							 index, dtype->type_ident, index);
		else
			appendStringInfo(&str,
							 "#define KVAR_%u\t"
							 "pg_%s_vref(kcs,%u,get_global_id(0))\n",
							 index, dtype->type_ident, index);
		index++;
	}

	/* columns to be referenced */
	appendStringInfo(&str,
					 "\n"
					 "static __constant cl_ushort pg_used_vars[]={");
	foreach (cell, context->used_vars)
	{
		Var	   *var = lfirst(cell);

		appendStringInfo(&str, "%s%u",
						 cell == list_head(context->used_vars) ? "" : ", ",
						 var->varattno);
	}
	appendStringInfo(&str, "};\n\n");

	/* qualifier definition with row-store */
	appendStringInfo(&str,
					 "__kernel void\n"
					 "gpuscan_qual_cs(__global kern_gpuscan *gpuscan,\n"
					 "                __global kern_parambuf *kparams,\n"
					 "                __global kern_column_store *kcs,\n"
					 "                __global kern_toastbuf *toast,\n"
					 "                __local void *local_workmem)\n"
					 "{\n"
					 "  pg_bool_t   rc;\n"
					 "  cl_int      errcode;\n"
					 "\n"
					 "  gpuscan_local_init(local_workmem);\n"
					 "  if (get_global_id(0) < kcs->nrows)\n"
					 "    rc = %s;\n"
					 "  else\n"
					 "    rc.isnull = CL_TRUE;\n"
					 "  kern_set_error(!rc.isnull && rc.value != 0\n"
					 "                 ? StromError_Success\n"
					 "                 : StromError_RowFiltered);\n"
					 "  gpuscan_writeback_result(gpuscan);\n"
					 "}\n"
					 "\n"
					 "__kernel void\n"
					 "gpuscan_qual_rs_prep(__global kern_row_store *krs,\n"
					 "                     __global kern_column_store *kcs)\n"
					 "{\n"
					 "  kern_row_to_column_prep(krs,kcs,\n"
					 "                          lengthof(used_vars),\n"
					 "                          used_vars);\n"
					 "}\n"
					 "\n"
					 "__kernel void\n"
					 "gpuscan_qual_rs(__global kern_gpuscan *gpuscan,\n"
					 "                __global kern_parambuf *kparams,\n"
					 "                __global kern_row_store *krs,\n"
					 "                __global kern_column_store *kcs,\n"
					 "                __local void *local_workmem)\n"
					 "{\n"
					 "  kern_row_to_column(krs,kcs,\n"
					 "                     lengthof(used_vars),\n"
					 "                     used_vars,\n"
					 "                     local_workmem);\n"
					 "  gpuscan_qual_cs(gpuscan,kparams,kcs,\n"
					 "                  (kern_toastbuf *)krs,\n"
					 "                  local_workmem);\n"
					 "}\n", expr_code);
	return str.data;
}

static CustomPlan *
gpuscan_create_plan(PlannerInfo *root, CustomPath *best_path)
{
	RelOptInfo	   *rel = best_path->path.parent;
	GpuScanPath	   *gpath = (GpuScanPath *)best_path;
	GpuScanPlan	   *gscan;
	List		   *tlist;
	List		   *host_clauses;
	List		   *dev_clauses;
	char		   *kern_source;
	codegen_context	context;

	/*
	 * See the comments in create_scan_plan(). We may be able to omit
	 * projection of the table tuples, if possible.
	 */
	if (use_physical_tlist(root, rel))
	{
		tlist = build_physical_tlist(root, rel);
		if (tlist == NIL)
			tlist = build_path_tlist(root, best_path);
	}
	else
		tlist = build_path_tlist(root, best_path);

	/* it should be a base relation */
	Assert(rel->relid > 0);
	Assert(rel->relkind == RTE_RELATION);

	/* Sort clauses into best execution order */
	host_clauses = order_qual_clauses(root, gpath->host_quals);
	dev_clauses = order_qual_clauses(root, gpath->dev_quals);

	/* Reduce RestrictInfo list to bare expressions; ignore pseudoconstants */
	host_clauses = extract_actual_clauses(host_clauses, false);
	dev_clauses = extract_actual_clauses(dev_clauses, false);

	/* Replace any outer-relation variables with nestloop params */
	if (best_path->path.param_info)
	{
		host_clauses = (List *)
			replace_nestloop_params(root, (Node *) host_clauses);
		dev_clauses = (List *)
			replace_nestloop_params(root, (Node *) dev_clauses);
	}

	/*
	 * Construct OpenCL kernel code - A kernel code contains two forms of
	 * entrypoints; for row-store and column-store. OpenCL intermediator
	 * invoked proper kernel function according to the class of data store.
	 * Once a kernel function for row-store is called, it translates the
	 * data format into column-store, then kicks jobs for row-evaluation.
	 * This design is optimized to process column-oriented data format on
	 * the relation cache.
	 */
	kern_source = gpuscan_codegen_quals(gpath->dev_quals, &context);

	/*
	 * Construction of GpuScanPlan node; on top of CustomPlan node
	 */
	gscan = palloc0(sizeof(GpuScanPlan));
	gscan->cplan.plan.type = T_CustomPlan;
	gscan->cplan.plan.targetlist = tlist;
	gscan->cplan.plan.qual = host_clauses;
	gscan->cplan.plan.lefttree = NULL;
	gscan->cplan.plan.righttree = NULL;

	gscan->scanrelid = rel->relid;
	gscan->kern_source = kern_source;
	gscan->extra_flags = context->extra_flags;
	gscan->used_params = context->used_params;
	gscan->used_vars = context->used_vars;
	gscan->dev_clauses = dev_clauses;
	gscan->dev_attrs = gpath->dev_attrs;
	gscan->host_attrs = gpath->host_attrs;

	return &gscan->cplan;
}

/* copy from outfuncs.c */
static void
_outBitmapset(StringInfo str, const Bitmapset *bms)
{
	Bitmapset  *tmpset;
	int			x;

	appendStringInfoChar(str, '(');
	appendStringInfoChar(str, 'b');
	tmpset = bms_copy(bms);
	while ((x = bms_first_member(tmpset)) >= 0)
		appendStringInfo(str, " %d", x);
	bms_free(tmpset);
	appendStringInfoChar(str, ')');
}

/* copy from outfuncs.c */
static void
_outToken(StringInfo str, const char *s)
{
	if (s == NULL || *s == '\0')
	{
		appendStringInfoString(str, "<>");
		return;
	}

	/*
	 * Look for characters or patterns that are treated specially by read.c
	 * (either in pg_strtok() or in nodeRead()), and therefore need a
	 * protective backslash.
	 */
	/* These characters only need to be quoted at the start of the string */
	if (*s == '<' ||
		*s == '\"' ||
		isdigit((unsigned char) *s) ||
		((*s == '+' || *s == '-') &&
		 (isdigit((unsigned char) s[1]) || s[1] == '.')))
		appendStringInfoChar(str, '\\');
	while (*s)
	{
		/* These chars must be backslashed anywhere in the string */
		if (*s == ' ' || *s == '\n' || *s == '\t' ||
			*s == '(' || *s == ')' || *s == '{' || *s == '}' ||
			*s == '\\')
			appendStringInfoChar(str, '\\');
		appendStringInfoChar(str, *s++);
	}
}

static void
gpuscan_textout_path(StringInfo str, Node *node)
{
	GpuScanPath	   *pathnode = (GpuScanPath *) node;
	Bitmapset	   *tempset;
	char		   *temp;
	int				x;

	/* dev_quals */
	temp = nodeToString(pathnode->dev_quals);
	appendStringInfo(str, " :dev_quals %s", temp);
	pfree(tmep);

	/* host_quals */
	temp = nodeToString(pathnode->host_quals);
	appendStringInfo(str, " :host_quals %s", temp);
	pfree(temp);

	/* dev_attrs */
	appendStringInfo(str, " :dev_attrs");
	_outBitmapset(str, pathnode->dev_attrs);

	/* host_attrs */
	appendStringInfo(str, " :host_attrs");
	_outBitmapset(str, pathnode->host_attrs);
}

static void
gpuscan_set_plan_ref(PlannerInfo *root,
					 CustomPlan *custom_plan,
					 int rtoffset)
{
	GpuScanPlan	   *gscan = (GpuScanPlan *)custom_plan;

	gscan->scanrelid += rtoffset;
	gscan->cplan.plan.targetlist = (List *)
		fix_scan_expr(root, (List *)gscan->cplan.plan.targetlist, rtoffset);
	gscan->cplan.plan.qual = (List *)
		fix_scan_expr(root, (List *)gscan->cplan.plan.qual, rtoffset);
	gscan->used_vars = (List *)
		fix_scan_expr(root, (List *)gscan->used_vars, rtoffset);
	gscan->dev_clauses = (List *)
		fix_scan_expr(root, (List *)gscan->dev_clauses, rtoffset);
}

static void
gpuscan_finalize_plan(PlannerInfo *root,
					  CustomPlan *custom_plan,
					  Bitmapset **paramids,
					  Bitmapset **valid_params,
					  Bitmapset **scan_params)
{
	*paramids = bms_add_members(*paramids, *scan_params);
}

static  CustomPlanState *
gpuscan_begin(CustomPlan *node, EState *estate, int eflags)
{
	GpuScanPlan	   *gsplan = (GpuScanPlan *) node;
	Index			scanrelid = gsplan->scanrelid;
	GpuScanState   *gss;
	TupleDesc		tupdesc;
	char			namebuf[NAMEDATALEN+1];

	/* gpuscan should not have inner/outer plan now */
	Assert(outerPlan(custom_plan) == NULL);
    Assert(innerPlan(custom_plan) == NULL);

	/*
	 * create a state structure
	 */
	gss = palloc0(sizeof(GpuScanState));
	gss.cps.ps.type = T_CustomPlanState;
	gss.cps.ps.plan = (Plan *) node;
	gss.cps.ps.state = estate;

	/*
	 * create expression context
	 */
	ExecAssignExprContext(estate, &gss->cps.ps);

	/*
	 * initialize child expressions
	 */
	gss->cps.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist, &gss->cps.ps);
	gss->cps.ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual, &gss->cps.ps);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &gss->cps.ps);
	gss->scan_slot = ExecAllocTableSlot(&estate->es_tupleTable); // needed?

	/*
	 * initialize scan relation
	 */
	gss->scan_rel = ExecOpenScanRelation(estate, scanrelid, eflags);
	gss->scan_desc = heap_beginscan(gss->scan_rel,
									estate->es_snapshot,
									0,
									NULL);
	tupdesc = RelationGetDescr(gss->scan_rel);
	ExecSetSlotDescriptor(gss->scan_slot, tupdesc);

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&gss->cps.ps);
	if (tlist_matches_tupdesc(&gss->cps.ps,
							  node->plan.targetlist,
							  scanrelid,
							  tupdesc))
		gss->cps.ps.ps_ProjInfo = NULL;
	else
		ExecAssignProjectionInfo(&gss->cps.ps, tupdesc);

	/*
	 * OK, initialization of common part is over.
	 * Let's have GPU stuff initialization
	 */
	gss->scan_mode = GpuScanMode_HeapOnlyScan;
	snprintf(namebuf, sizeof(namebuf),
			 "gpuscan(pid:%u, datoid:%u, reloid:%u, rtindex:%u",
			 MyProcPid, MyDatabaseId,
			 RelationGetRelid(gss->scan_rel),
			 scanrelid);
	gss->shmcontext = pgstrom_shmem_context_create(namebuf);
	if (!gss->shmcontext)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("failed to create shared memory context")));
	// TODO: add resource tracking here

	gss->mqueue = pgstrom_create_queue();
	if (!gss->mqueue)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("failed to create message queue")));
	// TODO: add resource tracking here

	gss->parambuf = pgstrom_create_parambuf(gss->shmcontext, ...);

	if (gsplan->kern_source)
		gss->dprog_key = pgstrom_get_devprog_key(gsplan->kern_source,
												 gsplan->extra_flags);
	else
		gss->dprog_key = 0;
	// TODO: add resource tracking here








	return &gss->cps;
}

static TupleTableSlot *
gpuscan_exec(CustomPlanState *node)
{}

static Node *
gpuscan_exec_multi(CustomPlanState *node)
{
	elog(ERROR, "not implemented yet");
}

static void
gpuscan_end(CustomPlanState *node)
{}

static void
gpuscan_rescan(CustomPlanState *node)
{
	elog(ERROR, "not implemented yet");
}

static void
gpuscan_explain_rel(CustomPlanState *node, ExplainState *es)
{}

static void
gpuscan_explain(CustomPlanState *node, List *ancestors, ExplainState *es)
{}

static Bitmapset *
gpuscan_get_relids(CustomPlanState *node)
{
	GpuScanPlan	   *gsp = (GpuScanPlan *)node->cps.ps.plan;

	return bms_make_singleton(gsp->scanrelid);
}

static void
gpuscan_textout_plan(StringInfo str, const CustomPlan *node)
{
	GpuScanPlan	   *plannode = (GpuScanPlan *)node;
	char		   *temp;

	appendStringInfoChar(str, " :scanrelid %u", plannode->scanrelid);

	appendStringInfoChar(str, " :kern_source ");
	_outToken(str, plannode->kern_source);

	appendStringInfo(str, " :extra_flags %u", plannode->scanrelid);

	temp = nodeToString(plannode->used_params);
	appendStringInfo(str, " :used_params %s", temp);
	pfree(temp);

	temp = nodeToString(plannode->used_vars);
	appendStringInfo(str, " :used_vars %s", temp);
	pfree(temp);

	temp = nodeToString(plannode->dev_clauses);
	appendStringInfo(str, " :dev_clauses %s", temp);
	pfree(temp);

	appendStringInfo(str, " :dev_attrs ");
	_outBitmapset(str, plannode->dev_attrs);

	appendStringInfo(str, " :host_attrs ");
	_outBitmapset(str, plannode->host_attrs);
}

static CustomPlan *
gpuscan_copy_plan(const CustomPlan *from)
{
	GpuScanPlan	   *newnode = palloc(sizeof(GpuScanPlan));

	CopyCustomPlanCommon(from, newnode);
	newnode->scanrelid = from->scanrelid;
	newnode->used_params = copyObject(from->used_params);
	newnode->used_vars = copyObject(from->used_vars);
	newnode->extra_flags = from->extra_flags;
	newnode->dev_clauses = from->dev_clauses;
	newnode->dev_attrs = bms_copy(from->dev_attrs);
	newnode->host_attrs = bms_copy(from->host_attrs);

	return &newnode->cplan;
}

void
pgstrom_init_gpuscan(void)
{
	/* GUC definition */
	DefineCustomBoolVariable("pgstrom.enable_gpuscan",
							 "Enables the planner's use of GPU-scan plans.",
							 NULL,
							 &enable_gpuscan,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* setup path methods */
	gpuscan_path_methods.CustomName			= "GpuScan";
	gpuscan_path_methods.CreateCustomPlan	= gpuscan_create_plan;
	gpuscan_path_methods.TextOutCustomPath	= gpuscan_textout_path;

	/* setup plan methods */
	gpuscan_plan_methods.CustomName			= "GpuScan";
	gpuscan_plan_methods.SetCustomPlanRef	= gpuscan_set_plan_ref;
	gpuscan_plan_methods.SupportBackwardScan= NULL;
	gpuscan_plan_methods.FinalizeCustomPlan	= gpuscan_finalize_plan;
	gpuscan_plan_methods.BeginCustomPlan	= gpuscan_begin;
	gpuscan_plan_methods.ExecCustomPlan		= gpuscan_exec;
	gpuscan_plan_methods.MultiExecCustomPlan= gpuscan_exec_multi;
	gpuscan_plan_methods.EndCustomPlan		= gpuscan_end;
	gpuscan_plan_methods.ReScanCustomPlan	= gpuscan_rescan;
	gpuscan_plan_methods.ExplainCustomPlanTargetRel	= gpuscan_explain_rel;
	gpuscan_plan_methods.ExplainCustomPlan	= gpuscan_explain;
	gpuscan_plan_methods.GetRelidsCustomPlan= gpuscan_get_relids;
	gpuscan_plan_methods.GetSpecialCustomVar= NULL;
	gpuscan_plan_methods.TextOutCustomPlan	= gpuscan_textout_plan;
	gpuscan_plan_methods.CopyCustomPlan		= gpuscan_copy_plan;

	/* hook registration */
	add_scan_path_next = add_scan_path_hook;
	add_scan_path_hook = gpuscan_add_scan_path;
}