/*
 * opencl_catalog.h
 *
 * 
 *
 *
 */
#ifndef PGSTROM_OPENCL_CATALOG_H
#define PGSTROM_OPENCL_CATALOG_H

enum {
	GPUCMD_INVALID = 0,	/* end of commands */

	/*
	 * Const References
	 */
	GPUCMD_CONREF_BOOL,
	GPUCMD_CONREF_INT2,
	GPUCMD_CONREF_INT4,
	GPUCMD_CONREF_INT8,
	GPUCMD_CONREF_FLOAT4,
	GPUCMD_CONREF_FLOAT8,

	/*
	 * Variable References
	 */
	GPUCMD_VARREF_BOOL,
	GPUCMD_VARREF_INT2,
	GPUCMD_VARREF_INT4,
	GPUCMD_VARREF_INT8,
	GPUCMD_VARREF_FLOAT4,
	GPUCMD_VARREF_FLOAT8,

	/*
	 * Cast of data types
	 */
	GPUCMD_CAST_INT2_TO_INT4,
	GPUCMD_CAST_INT2_TO_INT8,
	GPUCMD_CAST_INT2_TO_FLOAT4,
	GPUCMD_CAST_INT2_TO_FLOAT8,
	GPUCMD_CAST_INT4_TO_INT2,
	GPUCMD_CAST_INT4_TO_INT8,
	GPUCMD_CAST_INT4_TO_FLOAT4,
	GPUCMD_CAST_INT4_TO_FLOAT8,
	GPUCMD_CAST_INT8_TO_INT2,
	GPUCMD_CAST_INT8_TO_INT4,
	GPUCMD_CAST_INT8_TO_FLOAT4,
	GPUCMD_CAST_INT8_TO_FLOAT8,
	GPUCMD_CAST_FLOAT4_TO_INT2,
	GPUCMD_CAST_FLOAT4_TO_INT4,
	GPUCMD_CAST_FLOAT4_TO_INT8,
	GPUCMD_CAST_FLOAT4_TO_FLOAT8,
	GPUCMD_CAST_FLOAT8_TO_INT2,
	GPUCMD_CAST_FLOAT8_TO_INT4,
	GPUCMD_CAST_FLOAT8_TO_INT8,
	GPUCMD_CAST_FLOAT8_TO_FLOAT4,





	
	
	
	
} GpuCmds;

#endif	/* PGSTROM_OPENCL_CATALOG_H */