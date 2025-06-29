/*-------------------------------------------------------------------------
 * llvmjit.h
 *	  LLVM JIT provider.
 *
 * Copyright (c) 2016-2019, PostgreSQL Global Development Group
 *
 * src/include/jit/llvmjit.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LLVMJIT_H
#define LLVMJIT_H

/*
 * To avoid breaking cpluspluscheck, allow including the file even when LLVM
 * is not available.
 */
#ifdef USE_LLVM

#include <llvm-c/Types.h>


/*
 * File needs to be includable by both C and C++ code, and include other
 * headers doing the same. Therefore wrap C portion in our own extern "C" if
 * in C++ mode.
 */
#ifdef __cplusplus
extern "C"
{
#endif


#include "fmgr.h"
#include "jit/jit.h"
#include "nodes/pg_list.h"
#include "access/tupdesc.h"


typedef struct LLVMJitContext
{
	JitContext	base;

	/* number of modules created */
	size_t		module_generation;

	/*
	 * The LLVM Context used by this JIT context. An LLVM context is reused
	 * across many compilations, but occasionally reset to prevent it using
	 * too much memory due to more and more types accumulating.
	 */
	LLVMContextRef llvm_context;

	/* current, "open for write", module */
	LLVMModuleRef module;

	/* is there any pending code that needs to be emitted */
	bool		compiled;

	/* # of objects emitted, used to generate non-conflicting names */
	int			counter;

	/* list of handles for code emitted via Orc */
	List	   *handles;
} LLVMJitContext;

/* llvm module containing information about types */
extern PGDLLIMPORT LLVMModuleRef llvm_types_module;

/* type and struct definitions */
extern LLVMTypeRef TypeParamBool;
extern LLVMTypeRef TypePGFunction;
extern LLVMTypeRef TypeSizeT;
extern LLVMTypeRef TypeStorageBool;

extern LLVMTypeRef StructNullableDatum;
extern LLVMTypeRef StructTupleDescData;
extern LLVMTypeRef StructHeapTupleData;
extern LLVMTypeRef StructHeapTupleHeaderData;
extern LLVMTypeRef StructMinimalTupleData;
extern LLVMTypeRef StructTupleTableSlot;
extern LLVMTypeRef StructHeapTupleTableSlot;
extern LLVMTypeRef StructMinimalTupleTableSlot;
extern LLVMTypeRef StructMemoryContextData;
extern LLVMTypeRef StructFunctionCallInfoData;
extern LLVMTypeRef StructExprContext;
extern LLVMTypeRef StructExprEvalStep;
extern LLVMTypeRef StructExprState;
extern LLVMTypeRef StructAggState;
extern LLVMTypeRef StructAggStatePerTransData;
extern LLVMTypeRef StructAggStatePerGroupData;

extern LLVMValueRef AttributeTemplate;
extern LLVMValueRef ExecEvalSubroutineTemplate;


extern void llvm_enter_fatal_on_oom(void);
extern void llvm_leave_fatal_on_oom(void);
extern bool llvm_in_fatal_on_oom(void);
extern void llvm_reset_after_error(void);
extern void llvm_assert_in_fatal_section(void);

extern LLVMJitContext *llvm_create_context(int jitFlags);
extern LLVMModuleRef llvm_mutable_module(LLVMJitContext *context);
extern char *llvm_expand_funcname(LLVMJitContext *context, const char *basename);
extern void *llvm_get_function(LLVMJitContext *context, const char *funcname);
extern void llvm_split_symbol_name(const char *name, char **modname, char **funcname);
extern LLVMTypeRef llvm_pg_var_type(const char *varname);
extern LLVMTypeRef llvm_pg_var_func_type(const char *varname);
extern LLVMValueRef llvm_pg_func(LLVMModuleRef mod, const char *funcname);
extern void llvm_copy_attributes(LLVMValueRef from, LLVMValueRef to);
extern LLVMValueRef llvm_function_reference(LLVMJitContext *context,
						LLVMBuilderRef builder,
						LLVMModuleRef mod,
						FunctionCallInfo fcinfo);

extern void llvm_inline_reset_caches(void);
extern void llvm_inline(LLVMModuleRef mod);

/*
 ****************************************************************************
 * Code generation functions.
 ****************************************************************************
 */
extern bool llvm_compile_expr(struct ExprState *state);
struct TupleTableSlotOps;
extern LLVMValueRef slot_compile_deform(struct LLVMJitContext *context, TupleDesc desc,
										const struct TupleTableSlotOps *ops, int natts);

/*
 ****************************************************************************
 * Extensions / Backward compatibility section of the LLVM C API
 * Error handling related functions.
 ****************************************************************************
 */
#if defined(HAVE_DECL_LLVMGETHOSTCPUNAME) && !HAVE_DECL_LLVMGETHOSTCPUNAME
/** Get the host CPU as a string. The result needs to be disposed with
  LLVMDisposeMessage. */
extern char *LLVMGetHostCPUName(void);
#endif

#if defined(HAVE_DECL_LLVMGETHOSTCPUFEATURES) && !HAVE_DECL_LLVMGETHOSTCPUFEATURES
/** Get the host CPU features as a string. The result needs to be disposed
  with LLVMDisposeMessage. */
extern char *LLVMGetHostCPUFeatures(void);
#endif

extern unsigned LLVMGetAttributeCountAtIndexPG(LLVMValueRef F, uint32 Idx);
extern LLVMTypeRef LLVMGetFunctionReturnType(LLVMValueRef r);
extern LLVMTypeRef LLVMGetFunctionType(LLVMValueRef r);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif							/* USE_LLVM */
#endif							/* LLVMJIT_H */
