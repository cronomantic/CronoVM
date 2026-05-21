/* CronoVM bitcode translator.
 *
 * Reads an LLVM bitcode file produced by Clang, validates it against the
 * CronoVM IR subset, and (with -o) emits a CronoVM .bin. Step-5 codegen
 * is intentionally narrow: a single function with straight-line scalar
 * arithmetic and a single ret. Anything else is rejected with a message.
 */

#include "cvm.h"   /* binary format constants only — does not link cvm */

#include <llvm-c/BitReader.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/Types.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bridge to the LLVM C++ API for SwitchInst case-value retrieval. The
 * upstream `LLVMGetSwitchCaseValue` wrapper landed in llvm-c/Core.h
 * very late (LLVM 20+); to support older but otherwise-fine toolchains
 * (Ubuntu 24.04's llvm-19, llvm-18) we ship our own thin shim in
 * llvm_c_compat.cpp. See that file for the implementation. */
extern LLVMValueRef cvm_llvm_get_switch_case_value(LLVMValueRef SI, unsigned i);

static int g_errors = 0;

static void diag_v(const char *func, const char *fmt, ...) {
    fprintf(stderr, "translator: ");
    if (func) fprintf(stderr, "in '%s': ", func);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    g_errors++;
}

#define ERR(func, ...) diag_v((func), __VA_ARGS__)

/* --- name helpers -------------------------------------------------------- */

static const char *value_name(LLVMValueRef v) {
    size_t n = 0;
    const char *s = LLVMGetValueName2(v, &n);
    return n ? s : "<unnamed>";
}

static const char *opcode_name(LLVMOpcode op) {
    switch (op) {
    case LLVMRet:           return "ret";
    case LLVMBr:            return "br";
    case LLVMSwitch:        return "switch";
    case LLVMIndirectBr:    return "indirectbr";
    case LLVMInvoke:        return "invoke";
    case LLVMUnreachable:   return "unreachable";
    case LLVMCallBr:        return "callbr";
    case LLVMFNeg:          return "fneg";
    case LLVMAdd:           return "add";
    case LLVMFAdd:          return "fadd";
    case LLVMSub:           return "sub";
    case LLVMFSub:          return "fsub";
    case LLVMMul:           return "mul";
    case LLVMFMul:          return "fmul";
    case LLVMUDiv:          return "udiv";
    case LLVMSDiv:          return "sdiv";
    case LLVMFDiv:          return "fdiv";
    case LLVMURem:          return "urem";
    case LLVMSRem:          return "srem";
    case LLVMFRem:          return "frem";
    case LLVMShl:           return "shl";
    case LLVMLShr:          return "lshr";
    case LLVMAShr:          return "ashr";
    case LLVMAnd:           return "and";
    case LLVMOr:            return "or";
    case LLVMXor:           return "xor";
    case LLVMAlloca:        return "alloca";
    case LLVMLoad:          return "load";
    case LLVMStore:         return "store";
    case LLVMGetElementPtr: return "getelementptr";
    case LLVMTrunc:         return "trunc";
    case LLVMZExt:          return "zext";
    case LLVMSExt:          return "sext";
    case LLVMFPToUI:        return "fptoui";
    case LLVMFPToSI:        return "fptosi";
    case LLVMUIToFP:        return "uitofp";
    case LLVMSIToFP:        return "sitofp";
    case LLVMFPTrunc:       return "fptrunc";
    case LLVMFPExt:         return "fpext";
    case LLVMPtrToInt:      return "ptrtoint";
    case LLVMIntToPtr:      return "inttoptr";
    case LLVMBitCast:       return "bitcast";
    case LLVMAddrSpaceCast: return "addrspacecast";
    case LLVMICmp:          return "icmp";
    case LLVMFCmp:          return "fcmp";
    case LLVMPHI:           return "phi";
    case LLVMCall:          return "call";
    case LLVMSelect:        return "select";
    case LLVMVAArg:         return "va_arg";
    case LLVMExtractElement:return "extractelement";
    case LLVMInsertElement: return "insertelement";
    case LLVMShuffleVector: return "shufflevector";
    case LLVMExtractValue:  return "extractvalue";
    case LLVMInsertValue:   return "insertvalue";
    case LLVMFreeze:        return "freeze";
    case LLVMFence:         return "fence";
    case LLVMAtomicCmpXchg: return "cmpxchg";
    case LLVMAtomicRMW:     return "atomicrmw";
    case LLVMResume:        return "resume";
    case LLVMLandingPad:    return "landingpad";
    case LLVMCleanupRet:    return "cleanupret";
    case LLVMCatchRet:      return "catchret";
    case LLVMCatchPad:      return "catchpad";
    case LLVMCleanupPad:    return "cleanuppad";
    case LLVMCatchSwitch:   return "catchswitch";
    default:                return "<unknown>";
    }
}

static const char *type_kind_name(LLVMTypeKind k) {
    switch (k) {
    case LLVMVoidTypeKind:           return "void";
    case LLVMHalfTypeKind:           return "half";
    case LLVMBFloatTypeKind:         return "bfloat";
    case LLVMFloatTypeKind:          return "float";
    case LLVMDoubleTypeKind:         return "double";
    case LLVMX86_FP80TypeKind:       return "x86_fp80";
    case LLVMFP128TypeKind:          return "fp128";
    case LLVMPPC_FP128TypeKind:      return "ppc_fp128";
    case LLVMLabelTypeKind:          return "label";
    case LLVMIntegerTypeKind:        return "integer";
    case LLVMFunctionTypeKind:       return "function";
    case LLVMStructTypeKind:         return "struct";
    case LLVMArrayTypeKind:          return "array";
    case LLVMPointerTypeKind:        return "pointer";
    case LLVMVectorTypeKind:         return "vector";
    case LLVMMetadataTypeKind:       return "metadata";
    case LLVMTokenTypeKind:          return "token";
    case LLVMScalableVectorTypeKind: return "scalable_vector";
    case LLVMX86_AMXTypeKind:        return "x86_amx";
    case LLVMTargetExtTypeKind:      return "target_ext";
    default:                         return "<unknown>";
    }
}

/* --- subset checks ------------------------------------------------------- */

static int type_in_subset(LLVMTypeRef t, char *err, size_t errlen) {
    LLVMTypeKind k = LLVMGetTypeKind(t);
    switch (k) {
    case LLVMVoidTypeKind:
    case LLVMLabelTypeKind:
    case LLVMPointerTypeKind:
    case LLVMFunctionTypeKind:
    case LLVMMetadataTypeKind:
        return 1;
    case LLVMIntegerTypeKind: {
        unsigned bits = LLVMGetIntTypeWidth(t);
        if (bits == 1 || bits == 8 || bits == 16 || bits == 32) return 1;
        if (bits == 64) {
            snprintf(err, errlen,
                     "i64 not yet supported (deferred to int64 opcodes)");
            return 0;
        }
        snprintf(err, errlen, "unsupported integer width: i%u", bits);
        return 0;
    }
    case LLVMArrayTypeKind:
        return type_in_subset(LLVMGetElementType(t), err, errlen);
    case LLVMStructTypeKind: {
        unsigned n = LLVMCountStructElementTypes(t);
        for (unsigned i = 0; i < n; ++i) {
            if (!type_in_subset(LLVMStructGetTypeAtIndex(t, i), err, errlen))
                return 0;
        }
        return 1;
    }
    case LLVMVectorTypeKind:
    case LLVMScalableVectorTypeKind:
        snprintf(err, errlen, "vector types not in the subset");
        return 0;
    case LLVMFloatTypeKind:
        /* IEEE 754 binary32 — first-class in the ISA (see CVM_OP_F* in
         * include/cvm.h). f32 values share the i32 register file, so no
         * separate register class is needed. */
        return 1;
    case LLVMDoubleTypeKind:
        /* `double` is rejected by design. The runtime header
         * cvm_float64.h provides software emulation for code that
         * genuinely needs it, but the ISA stays 32-bit. */
        snprintf(err, errlen,
                 "f64 not in the subset; use float, or include "
                 "cvm_float64.h for software emulation");
        return 0;
    case LLVMHalfTypeKind:
    case LLVMBFloatTypeKind:
    case LLVMX86_FP80TypeKind:
    case LLVMFP128TypeKind:
    case LLVMPPC_FP128TypeKind:
        snprintf(err, errlen,
                 "%s floating-point not in the subset (only f32 supported)",
                 type_kind_name(k));
        return 0;
    case LLVMTokenTypeKind:
    case LLVMX86_AMXTypeKind:
    case LLVMTargetExtTypeKind:
    default:
        snprintf(err, errlen,
                 "type kind '%s' not in the subset", type_kind_name(k));
        return 0;
    }
}

static int opcode_in_subset(LLVMOpcode op) {
    switch (op) {
    case LLVMRet: case LLVMBr: case LLVMSwitch: case LLVMUnreachable:
    case LLVMAdd: case LLVMSub: case LLVMMul:
    case LLVMUDiv: case LLVMSDiv: case LLVMURem: case LLVMSRem:
    case LLVMShl: case LLVMLShr: case LLVMAShr:
    case LLVMAnd: case LLVMOr: case LLVMXor:
    case LLVMAlloca: case LLVMLoad: case LLVMStore: case LLVMGetElementPtr:
    case LLVMTrunc: case LLVMZExt: case LLVMSExt:
    case LLVMPtrToInt: case LLVMIntToPtr: case LLVMBitCast:
    case LLVMICmp: case LLVMPHI: case LLVMCall: case LLVMSelect:
    case LLVMExtractValue: case LLVMInsertValue:
    /* f32 ops — see translator's float codegen. FRem deliberately
     * excluded (no FREM opcode; users wanting fmod can call libc-style
     * helper from a future runtime header). FPExt/FPTrunc excluded
     * because they only apply to double, which we reject. */
    case LLVMFAdd: case LLVMFSub: case LLVMFMul: case LLVMFDiv:
    case LLVMFNeg: case LLVMFCmp:
    case LLVMSIToFP: case LLVMUIToFP: case LLVMFPToSI: case LLVMFPToUI:
        return 1;
    default:
        return 0;
    }
}

static const char *reject_reason(LLVMOpcode op) {
    switch (op) {
    case LLVMFNeg:
    case LLVMFAdd: case LLVMFSub: case LLVMFMul: case LLVMFDiv: case LLVMFRem:
    case LLVMFCmp:
    case LLVMFPToUI: case LLVMFPToSI: case LLVMUIToFP: case LLVMSIToFP:
    case LLVMFPTrunc: case LLVMFPExt:
        return "floating-point operations not yet supported";
    case LLVMInvoke: case LLVMResume: case LLVMLandingPad:
    case LLVMCleanupRet: case LLVMCatchRet: case LLVMCatchPad:
    case LLVMCleanupPad: case LLVMCatchSwitch:
        return "exception-handling instructions not in the subset";
    case LLVMAtomicCmpXchg: case LLVMAtomicRMW: case LLVMFence:
        return "atomic / fence operations not in the subset";
    case LLVMExtractElement: case LLVMInsertElement: case LLVMShuffleVector:
        return "vector instructions not in the subset";
    case LLVMVAArg:
        return "va_arg not in the subset";
    case LLVMIndirectBr: case LLVMCallBr:
        return "indirectbr / callbr not in the subset";
    case LLVMAddrSpaceCast:
        return "non-default address spaces not supported";
    case LLVMFreeze:
        return "freeze not yet supported";
    default:
        return "instruction not in the subset";
    }
}

/* --- module walking ------------------------------------------------------ */

static void validate_function(LLVMValueRef fn) {
    char err[256];
    const char *fn_name = value_name(fn);

    LLVMTypeRef fnty = LLVMGlobalGetValueType(fn);
    LLVMTypeRef ret_ty = LLVMGetReturnType(fnty);
    if (!type_in_subset(ret_ty, err, sizeof(err)))
        ERR(fn_name, "return type rejected: %s", err);

    unsigned param_count = LLVMCountParams(fn);
    for (unsigned i = 0; i < param_count; ++i) {
        LLVMTypeRef pt = LLVMTypeOf(LLVMGetParam(fn, i));
        if (!type_in_subset(pt, err, sizeof(err)))
            ERR(fn_name, "parameter %u rejected: %s", i, err);
    }

    LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn);
    for (; bb; bb = LLVMGetNextBasicBlock(bb)) {
        LLVMValueRef inst = LLVMGetFirstInstruction(bb);
        for (; inst; inst = LLVMGetNextInstruction(inst)) {
            LLVMOpcode op = LLVMGetInstructionOpcode(inst);
            if (!opcode_in_subset(op))
                ERR(fn_name, "%s: %s",
                    opcode_name(op), reject_reason(op));

            LLVMTypeRef ity = LLVMTypeOf(inst);
            if (!type_in_subset(ity, err, sizeof(err)))
                ERR(fn_name, "%s produces unsupported type: %s",
                    opcode_name(op), err);
        }
    }
}

static void print_function_summary(LLVMValueRef fn) {
    LLVMTypeRef fty = LLVMGlobalGetValueType(fn);
    LLVMTypeRef rty = LLVMGetReturnType(fty);

    printf("function %s(", value_name(fn));
    unsigned n = LLVMCountParams(fn);
    for (unsigned i = 0; i < n; ++i) {
        char *t = LLVMPrintTypeToString(LLVMTypeOf(LLVMGetParam(fn, i)));
        printf("%s%s", t, (i + 1 < n) ? ", " : "");
        LLVMDisposeMessage(t);
    }
    char *rs = LLVMPrintTypeToString(rty);
    printf(") -> %s ", rs);
    LLVMDisposeMessage(rs);

    if (LLVMIsDeclaration(fn)) { printf("[declaration]\n"); return; }

    unsigned bbs = LLVMCountBasicBlocks(fn);
    unsigned ins = 0;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn);
         bb; bb = LLVMGetNextBasicBlock(bb))
    {
        for (LLVMValueRef i = LLVMGetFirstInstruction(bb);
             i; i = LLVMGetNextInstruction(i))
            ++ins;
    }
    printf("[%u block%s, %u instruction%s]\n",
           bbs, bbs == 1 ? "" : "s",
           ins, ins == 1 ? "" : "s");
}

/* --- globals ------------------------------------------------------------- */

struct cg_global {
    LLVMValueRef value;
    uint32_t     offset;     /* heap offset assigned at layout time */
    uint32_t     size;       /* bytes occupied on the heap */
};

struct cg_func;   /* fully defined in the codegen section below */

struct cg_globals {
    LLVMTargetDataRef td;
    struct cg_global *items;
    int               count;
    int               cap;
    uint8_t          *data_bytes;
    uint32_t          data_size;
    uint32_t          data_cap;

    /* Function table (not owned). Set before cg_collect_globals so
     * function values used as constant initialisers can be serialised
     * as their FUNCS-table indices. NULL when no functions have been
     * enumerated yet. */
    const struct cg_func *funcs;
    int                   func_count;
};

static int cg_globals_lookup(const struct cg_globals *g, LLVMValueRef v) {
    for (int i = 0; i < g->count; ++i)
        if (g->items[i].value == v) return i;
    return -1;
}

static int cg_globals_append(struct cg_globals *g, LLVMValueRef v,
                             uint32_t offset, uint32_t size)
{
    if (g->count == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 8;
        g->items = (struct cg_global *)realloc(g->items,
                            g->cap * sizeof(*g->items));
        if (!g->items) { perror("realloc"); return 1; }
    }
    g->items[g->count].value  = v;
    g->items[g->count].offset = offset;
    g->items[g->count].size   = size;
    g->count++;
    return 0;
}

static void cg_data_reserve(struct cg_globals *g, uint32_t need) {
    if (g->data_cap >= need) return;
    uint32_t cap = g->data_cap ? g->data_cap : 64;
    while (cap < need) cap *= 2;
    g->data_bytes = (uint8_t *)realloc(g->data_bytes, cap);
    if (!g->data_bytes) { perror("realloc"); exit(1); }
    memset(g->data_bytes + g->data_cap, 0, cap - g->data_cap);
    g->data_cap = cap;
}

/* Forward declaration: cg_func is fully defined later, but we only
 * need its `value` field, accessed through a typed pointer. */
struct cg_func {
    LLVMValueRef value;
    uint32_t     entry_offset;
    uint32_t     frame_size;
};

static int serialize_function_index(const struct cg_globals *g,
                                    LLVMValueRef fn,
                                    uint8_t *out, uint32_t off)
{
    if (!g->funcs) return 1;
    int fidx = -1;
    for (int i = 0; i < g->func_count; ++i)
        if (g->funcs[i].value == fn) { fidx = i; break; }
    if (fidx < 0) return 1;
    /* Function "address" = its FUNCS-table index + 1, written as a 32-bit
     * LE integer. CALLR Rd reads R[d] and looks up FUNCS[R[d]] at run
     * time; FUNCS[0] is the reserved null-function-pointer slot, so user
     * functions live at FUNCS[1..N] and a NULL fn-ptr is naturally 0. */
    uint32_t enc = (uint32_t)fidx + 1u;
    out[off + 0] = (uint8_t)(enc);
    out[off + 1] = (uint8_t)(enc >> 8);
    out[off + 2] = (uint8_t)(enc >> 16);
    out[off + 3] = (uint8_t)(enc >> 24);
    return 0;
}

static int serialize_constant(const struct cg_globals *g, LLVMValueRef c,
                              uint8_t *out, uint32_t off, uint32_t cap)
{
    LLVMTypeRef ty = LLVMTypeOf(c);
    uint32_t sz = (uint32_t)LLVMABISizeOfType(g->td, ty);
    if (off + sz > cap) return 1;

    if (LLVMIsAConstantAggregateZero(c) || LLVMIsAConstantPointerNull(c)) {
        /* out is calloc/zeroed already; nothing to write. */
        return 0;
    }
    if (LLVMIsAConstantInt(c)) {
        unsigned long long v = LLVMConstIntGetZExtValue(c);
        for (uint32_t b = 0; b < sz && b < 8; ++b)
            out[off + b] = (uint8_t)(v >> (b * 8));
        return 0;
    }
    if (LLVMIsAConstantFP(c)) {
        /* Only float (binary32) is in the subset; the type-subset check
         * upstream rejects anything else, but assert here so a bad caller
         * can't slip a double past us. */
        if (LLVMGetTypeKind(ty) != LLVMFloatTypeKind || sz != 4) return 1;
        LLVMBool lossy = 0;
        double   d     = LLVMConstRealGetDouble(c, &lossy);
        float    f     = (float)d;
        uint32_t bits;
        memcpy(&bits, &f, sizeof bits);
        out[off + 0] = (uint8_t)(bits      );
        out[off + 1] = (uint8_t)(bits >>  8);
        out[off + 2] = (uint8_t)(bits >> 16);
        out[off + 3] = (uint8_t)(bits >> 24);
        return 0;
    }
    if (LLVMIsAFunction(c)) {
        /* A function value embedded in a constant initialiser — typically
         * a static dispatch table. We need the function's FUNCS-index,
         * which means cg_collect_globals must run *after* the function
         * table has been built. */
        if (sz < 4) return 1;
        return serialize_function_index(g, c, out, off);
    }
    if (LLVMGetTypeKind(ty) == LLVMArrayTypeKind) {
        LLVMTypeRef elem = LLVMGetElementType(ty);
        uint32_t   esz   = (uint32_t)LLVMABISizeOfType(g->td, elem);
        unsigned   n     = LLVMGetArrayLength(ty);
        for (unsigned i = 0; i < n; ++i) {
            LLVMValueRef e = LLVMGetAggregateElement(c, i);
            if (!e) return 1;
            if (serialize_constant(g, e, out, off + i * esz, cap) != 0)
                return 1;
        }
        return 0;
    }
    if (LLVMGetTypeKind(ty) == LLVMStructTypeKind) {
        unsigned n = LLVMCountStructElementTypes(ty);
        for (unsigned i = 0; i < n; ++i) {
            LLVMValueRef e = LLVMGetAggregateElement(c, i);
            if (!e) return 1;
            uint32_t fo = (uint32_t)LLVMOffsetOfElement(g->td, ty, i);
            if (serialize_constant(g, e, out, off + fo, cap) != 0)
                return 1;
        }
        return 0;
    }
    return 1;
}

static int cg_collect_globals(struct cg_globals *g, LLVMModuleRef mod) {
    char err[256];
    uint32_t cursor = 0;
    for (LLVMValueRef gv = LLVMGetFirstGlobal(mod);
         gv; gv = LLVMGetNextGlobal(gv))
    {
        if (!LLVMGlobalGetValueType(gv)) continue;
        if (LLVMIsDeclaration(gv)) {
            ERR(NULL, "external global '%s' has no initializer (extern not supported)",
                value_name(gv));
            return 1;
        }

        LLVMTypeRef ty = LLVMGlobalGetValueType(gv);
        if (!type_in_subset(ty, err, sizeof(err))) {
            ERR(NULL, "global '%s' rejected: %s", value_name(gv), err);
            return 1;
        }

        uint32_t size  = (uint32_t)LLVMABISizeOfType(g->td, ty);
        uint32_t align = (uint32_t)LLVMABIAlignmentOfType(g->td, ty);
        if (align == 0) align = 1;
        cursor = (cursor + align - 1) & ~(align - 1);

        cg_data_reserve(g, cursor + size);
        LLVMValueRef init = LLVMGetInitializer(gv);
        if (init && serialize_constant(g, init,
                                       g->data_bytes, cursor, g->data_cap) != 0) {
            ERR(NULL, "global '%s': unsupported initializer shape "
                      "(or function-pointer initialiser without a "
                      "definition for the referenced function)",
                value_name(gv));
            return 1;
        }
        if (cg_globals_append(g, gv, cursor, size) != 0) return 1;
        cursor += size;
        if (cursor > g->data_size) g->data_size = cursor;
    }
    return 0;
}

static void cg_globals_dispose(struct cg_globals *g) {
    free(g->items);
    free(g->data_bytes);
    if (g->td) LLVMDisposeTargetData(g->td);
    memset(g, 0, sizeof(*g));
}

/* --- codegen ------------------------------------------------------------- */

struct cg_fixup {
    uint32_t inst_index;
    LLVMBasicBlockRef target;
    int      shift;     /* bit position of the imm field */
    int      bits;      /* width of the imm field (8 or 24) */
};

/* Switch jump-table entry that points at a basic block. The dispatch
 * sequence emitted by the table form of LLVMSwitch reads each entry
 * with LDW and JMPRs to the absolute instruction index, so we resolve
 * these *after* branch relaxation (when block_offsets is final) and
 * write the absolute index into globals.data_bytes at `data_off`. */
struct cg_table_fixup {
    uint32_t          data_off;     /* byte offset into globals.data_bytes */
    LLVMBasicBlockRef target;
};

struct cg_import {
    LLVMValueRef value;     /* the LLVM declaration */
    const char  *name;      /* points into LLVM-owned storage */
};

/* struct cg_func is defined earlier (before cg_globals) so that the
 * constant-initialiser serialiser can map function values in DATA to
 * their FUNCS-table indices. Indices into this table double as CALL
 * imm24 operands; the loader resolves them via FUNCS at run time. */

/* One entry per LLVMAlloca in the entry block (only static, entry-block
 * allocas are supported). offset is in bytes from SP after prologue. */
struct cg_alloca {
    LLVMValueRef value;
    uint32_t     offset;
};

/* Scratch register reserved by the codegen for prologue/epilogue/CALL
 * sequences (frame_size constants, spill addresses, stack-arg pointers).
 * Never holds an SSA value, so it doesn't need to be spilled across CALLs. */
#define CG_REG_SP      255u
#define CG_REG_SCRATCH 254u
#define CG_REG_ZERO    253u   /* dedicated register holding 0 across the
                                * whole function. Sits OUTSIDE the
                                * spillable SSA range so a callee's body
                                * never overwrites it; every function's
                                * prologue MOVIs it to 0, so on return
                                * the caller's R253 is still 0 (the
                                * callee just re-set it on entry). */
#define CG_MAX_SSA_REG 252u   /* SSA values use R8..R252 (R253=zero,
                                * R254=scratch, R255=SP) */

/* --- Liveness bitset ---------------------------------------------------- */
/* 256 bits, indexed by (register - 8). Covers any pre-allocated SSA
 * register R8..R253 (the spillable range) with room to spare. */
#define CG_BITS_W 4

typedef struct { uint64_t w[CG_BITS_W]; } cg_bits;

static inline void cg_bits_clear(cg_bits *b) {
    for (int i = 0; i < CG_BITS_W; ++i) b->w[i] = 0;
}
static inline void cg_bits_set(cg_bits *b, unsigned bit) {
    b->w[bit >> 6] |= (uint64_t)1 << (bit & 63);
}
static inline void cg_bits_clear_bit(cg_bits *b, unsigned bit) {
    b->w[bit >> 6] &= ~((uint64_t)1 << (bit & 63));
}
static inline int cg_bits_test(const cg_bits *b, unsigned bit) {
    return (int)((b->w[bit >> 6] >> (bit & 63)) & 1u);
}
static inline void cg_bits_or(cg_bits *dst, const cg_bits *src) {
    for (int i = 0; i < CG_BITS_W; ++i) dst->w[i] |= src->w[i];
}
static inline void cg_bits_andnot(cg_bits *dst, const cg_bits *src) {
    for (int i = 0; i < CG_BITS_W; ++i) dst->w[i] &= ~src->w[i];
}
static inline int cg_bits_eq(const cg_bits *a, const cg_bits *b) {
    for (int i = 0; i < CG_BITS_W; ++i)
        if (a->w[i] != b->w[i]) return 0;
    return 1;
}

/* For each LLVMCall instruction, the set of SSA registers live immediately
 * after the call returns. Drives the per-call spill in the LLVMCall handler:
 * only registers in this set need to be preserved across the call. */
struct cg_call_live {
    LLVMValueRef inst;
    cg_bits      live_after;
};

struct cg {
    /* Module-level state, persists across all functions. */
    uint32_t *code;
    uint32_t  count;
    uint32_t  cap;

    struct cg_globals *globals;       /* module-wide; not owned */

    /* Imports collected as we encounter cvm_sys_* calls. Each unique callee
     * gets a stable syscall_id (its index in this table). */
    struct cg_import *imports;
    int               import_count;
    int               import_cap;

    /* User-defined function table; index = CALL imm24. */
    struct cg_func *funcs;
    int             func_count;
    int             func_cap;

    int has_calls;          /* did codegen ever emit a CALL? */
    int funcs_referenced;   /* was any function's address taken as a value?
                             * Such a pointer is a FUNCS index, so the table
                             * must be emitted even when no CALL/CALLR exists
                             * — e.g. a function handed to the host via a
                             * syscall for the host to invoke with cvm_call. */

    /* Per-function scratch — reset by cg_reset_function_state. */
    LLVMValueRef *vals;     /* SSA value -> physical register, parallel arrays */
    uint8_t      *regs;
    int           map_count;
    int           map_cap;
    int           next_reg;

    LLVMBasicBlockRef *blocks;
    uint32_t          *block_offsets;
    int                block_count;
    int                block_cap;

    struct cg_fixup *fixups;
    int              fixup_count;
    int              fixup_cap;

    /* Switch jump-table entries (see cg_table_fixup). Reset per
     * function; resolved in cg_function after branch relaxation. */
    struct cg_table_fixup *table_fixups;
    int                    table_fixup_count;
    int                    table_fixup_cap;

    /* Allocas in the current function (entry block, static size). */
    struct cg_alloca *allocas;
    int               alloca_count;
    int               alloca_cap;

    /* Liveness analysis (per function, recomputed in cg_function). Indexed
     * by block index in `cg->blocks`. live_cap tracks the buffer size so
     * we can reuse the allocation across functions and only realloc when
     * a function has more blocks than any prior one. */
    cg_bits *bb_live_in;
    cg_bits *bb_live_out;
    int      live_cap;

    /* Per-CALL "live-after" set: for each LLVMCall instruction, the SSA
     * registers (bit i ↔ R(8+i)) that hold values needed after the call
     * returns. The CALL handler spills only these registers instead of
     * everything in [8, ssa_reg_high). */
    struct cg_call_live *call_lives;
    int                  call_live_count;
    int                  call_live_cap;

    uint32_t          alloca_bytes;   /* total bytes used by alloca area */
    uint32_t          spill_bytes;    /* spill_slot_count * 4 — see below. */
    uint32_t          frame_bytes;    /* alloca_bytes + spill_bytes */
    int               ssa_reg_high;   /* next_reg snapshot after pre-alloc.
                                       * Spilling around CALL only covers
                                       * R8..R(ssa_reg_high-1). Constants
                                       * and per-instruction temps are
                                       * re-materialised on each use, so
                                       * they don't need to survive a CALL. */

    /* Compacted spill layout: only SSA regs that are live across at
     * least one call get a slot. `ever_spilled` is the union of every
     * call's live-after set; `slot_of[bit]` maps a spill bit
     * (reg - 8) to its compact slot index, or 0xFF if the reg never
     * crossed a call. `spill_slot_count` is the number of distinct
     * slots — `spill_bytes = spill_slot_count * 4`. The mapping is
     * recomputed in `cg_function` after `cg_compute_call_liveouts`. */
    cg_bits           ever_spilled;
    uint8_t           slot_of[256];
    int               spill_slot_count;

    uint8_t           zero_reg;       /* register holding 0, for branch tests */
    LLVMBasicBlockRef cur_block;      /* updated during emit walk */

    /* Block-local SSA register reuse pool (see cg_pre_alloc_function).
     * Reset at every block boundary in pre-alloc; cleared again at the
     * end of pre-alloc so the emit phase doesn't accidentally pop a
     * freed reg and trample on per-block transient materialisation.
     * Bounded by the spillable range (R8..R252 = 245 entries). */
    uint8_t           free_list[256];
    int               free_list_count;

    int           had_error;
    const char   *fn_name;
};

static int cg_func_lookup(const struct cg *cg, LLVMValueRef v) {
    for (int i = 0; i < cg->func_count; ++i)
        if (cg->funcs[i].value == v) return i;
    return -1;
}

static int cg_func_append(struct cg *cg, LLVMValueRef v) {
    if (cg->func_count == cg->func_cap) {
        cg->func_cap = cg->func_cap ? cg->func_cap * 2 : 8;
        cg->funcs = (struct cg_func *)realloc(cg->funcs,
                            cg->func_cap * sizeof(*cg->funcs));
        if (!cg->funcs) { perror("realloc"); return 1; }
    }
    cg->funcs[cg->func_count].value        = v;
    cg->funcs[cg->func_count].entry_offset = 0;
    cg->funcs[cg->func_count].frame_size   = 0;
    return cg->func_count++;
}

static int cg_alloca_append(struct cg *cg, LLVMValueRef v, uint32_t off) {
    if (cg->alloca_count == cg->alloca_cap) {
        cg->alloca_cap = cg->alloca_cap ? cg->alloca_cap * 2 : 4;
        cg->allocas = (struct cg_alloca *)realloc(cg->allocas,
                            cg->alloca_cap * sizeof(*cg->allocas));
        if (!cg->allocas) { perror("realloc"); return 1; }
    }
    cg->allocas[cg->alloca_count].value  = v;
    cg->allocas[cg->alloca_count].offset = off;
    cg->alloca_count++;
    return 0;
}

static void cg_reset_function_state(struct cg *cg) {
    cg->map_count = 0;
    cg->next_reg = 8;
    cg->block_count = 0;
    cg->fixup_count = 0;
    cg->alloca_count = 0;
    cg->alloca_bytes = 0;
    cg->spill_bytes = 0;
    cg->frame_bytes = 0;
    cg->ssa_reg_high = 8;
    cg->zero_reg = 0;
    cg->cur_block = NULL;
    cg->fn_name = NULL;
    cg->call_live_count = 0;
    cg_bits_clear(&cg->ever_spilled);
    memset(cg->slot_of, 0xFF, sizeof cg->slot_of);
    cg->spill_slot_count = 0;
    cg->table_fixup_count = 0;
}

static int cg_import_lookup_or_add(struct cg *cg, LLVMValueRef callee,
                                   const char *name)
{
    for (int i = 0; i < cg->import_count; ++i)
        if (cg->imports[i].value == callee) return i;
    if (cg->import_count == cg->import_cap) {
        cg->import_cap = cg->import_cap ? cg->import_cap * 2 : 4;
        cg->imports = (struct cg_import *)realloc(cg->imports,
                            cg->import_cap * sizeof(*cg->imports));
        if (!cg->imports) { perror("realloc"); exit(1); }
    }
    cg->imports[cg->import_count].value = callee;
    cg->imports[cg->import_count].name  = name;
    return cg->import_count++;
}

static uint32_t enc_r(uint8_t op, uint8_t a, uint8_t b, uint8_t c) {
    return (uint32_t)op
         | ((uint32_t)a << 8)
         | ((uint32_t)b << 16)
         | ((uint32_t)c << 24);
}
static uint32_t enc_i16(uint8_t op, uint8_t a, int16_t imm) {
    return (uint32_t)op
         | ((uint32_t)a << 8)
         | ((uint32_t)(uint16_t)imm << 16);
}
static uint32_t enc_i24(uint8_t op, int32_t imm) {
    return (uint32_t)op | (((uint32_t)imm & 0xFFFFFFu) << 8);
}
static uint32_t enc_br(uint8_t op, uint8_t a, uint8_t b, int8_t off) {
    return (uint32_t)op
         | ((uint32_t)a << 8)
         | ((uint32_t)b << 16)
         | ((uint32_t)(uint8_t)off << 24);
}

static void cg_emit(struct cg *cg, uint32_t inst) {
    if (cg->count == cg->cap) {
        cg->cap = cg->cap ? cg->cap * 2 : 64;
        cg->code = (uint32_t *)realloc(cg->code, cg->cap * sizeof(uint32_t));
        if (!cg->code) { perror("realloc"); exit(1); }
    }
    cg->code[cg->count++] = inst;
}

static uint8_t cg_alloc_reg(struct cg *cg) {
    /* Prefer a previously-freed reg from the block-local pool — see
     * cg_pre_alloc_function. The pool is empty during emit-phase
     * transient allocation (cg_reg_for for constants/globals), so
     * those still bump next_reg as before. */
    if (cg->free_list_count > 0) {
        return cg->free_list[--cg->free_list_count];
    }
    if (cg->next_reg > (int)CG_MAX_SSA_REG) {
        ERR(cg->fn_name, "ran out of registers (R254/R255 are reserved as "
                         "scratch and SP)");
        cg->had_error = 1;
        return 0;
    }
    return (uint8_t)cg->next_reg++;
}

static void cg_free_reg(struct cg *cg, uint8_t r) {
    if (cg->free_list_count
        < (int)(sizeof(cg->free_list) / sizeof(cg->free_list[0])))
    {
        cg->free_list[cg->free_list_count++] = r;
    }
}

/* Forward decl: defined further down with the prologue/stack helpers. */
static void cg_emit_load_const32(struct cg *cg, uint8_t rd, int32_t imm);

static int cg_lookup(struct cg *cg, LLVMValueRef v) {
    for (int i = 0; i < cg->map_count; ++i)
        if (cg->vals[i] == v) return i;
    return -1;
}

static uint8_t cg_assign(struct cg *cg, LLVMValueRef v, uint8_t r) {
    if (cg->map_count == cg->map_cap) {
        cg->map_cap = cg->map_cap ? cg->map_cap * 2 : 16;
        cg->vals = (LLVMValueRef *)realloc(cg->vals,
                                           cg->map_cap * sizeof(*cg->vals));
        cg->regs = (uint8_t *)realloc(cg->regs,
                                      cg->map_cap * sizeof(*cg->regs));
        if (!cg->vals || !cg->regs) { perror("realloc"); exit(1); }
    }
    cg->vals[cg->map_count] = v;
    cg->regs[cg->map_count] = r;
    cg->map_count++;
    return r;
}

/* Static byte offset of a constant-expression GEP. Every index of a
 * ConstantExpr GEP is a compile-time constant, so the whole offset folds.
 * Mirrors the index-walk in the LLVMGetElementPtr instruction handler.
 * Returns 0 on success (offset in *out), nonzero on an unsupported shape. */
static int cg_const_gep_offset(struct cg *cg, LLVMValueRef gep, long long *out) {
    LLVMTypeRef cur_ty = LLVMGetGEPSourceElementType(gep);
    if (!cur_ty) return 1;
    long long off = 0;
    unsigned  nidx = LLVMGetNumIndices(gep);
    for (unsigned k = 0; k < nidx; ++k) {
        LLVMValueRef idx_v = LLVMGetOperand(gep, k + 1);
        if (!LLVMIsAConstantInt(idx_v)) return 1;
        if (k == 0) {
            long long ci = LLVMConstIntGetSExtValue(idx_v);
            off += ci * (long long)LLVMABISizeOfType(cg->globals->td, cur_ty);
        } else if (LLVMGetTypeKind(cur_ty) == LLVMArrayTypeKind) {
            LLVMTypeRef et = LLVMGetElementType(cur_ty);
            long long ci = LLVMConstIntGetSExtValue(idx_v);
            off += ci * (long long)LLVMABISizeOfType(cg->globals->td, et);
            cur_ty = et;
        } else if (LLVMGetTypeKind(cur_ty) == LLVMStructTypeKind) {
            unsigned fi = (unsigned)LLVMConstIntGetZExtValue(idx_v);
            off += (long long)LLVMOffsetOfElement(cg->globals->td, cur_ty, fi);
            cur_ty = LLVMStructGetTypeAtIndex(cur_ty, fi);
        } else {
            return 1;
        }
    }
    *out = off;
    return 0;
}

/* Look up the physical register holding `v`. For constants, materialise a
 * fresh register at the call site each time — this is wasteful in registers
 * but always correct across multi-block control flow (a cached constant from
 * one block may not dominate uses in another). A future pass can hoist
 * shared constants to the function prologue. */
static uint8_t cg_reg_for(struct cg *cg, LLVMValueRef v) {
    int idx = cg_lookup(cg, v);
    if (idx >= 0) return cg->regs[idx];

    if (LLVMIsAConstantInt(v)) {
        long long imm = LLVMConstIntGetSExtValue(v);
        if (imm < INT32_MIN || imm > INT32_MAX) {
            ERR(cg->fn_name,
                "constant %lld is wider than i32 (i64 not yet supported)",
                imm);
            cg->had_error = 1;
            return 0;
        }
        uint8_t r = cg_alloc_reg(cg);
        if (cg->had_error) return 0;
        cg_emit_load_const32(cg, r, (int32_t)imm);
        return r;
    }

    if (LLVMIsAConstantPointerNull(v)) {
        uint8_t r = cg_alloc_reg(cg);
        if (cg->had_error) return 0;
        cg_emit(cg, enc_i16(CVM_OP_MOVI, r, 0));
        return r;
    }

    /* f32 constants. LLVM stores them as `double` internally — the C API
     * gives a `double`, so we cast to `float` and reinterpret to i32 to
     * recover the IEEE binary32 bit pattern that goes into the register.
     * The narrowing is safe: the value originally came from a `float`
     * literal, so the round-trip is exact for all but the most twisted
     * IR (which the type subset rejects upstream anyway). */
    if (LLVMIsAConstantFP(v)) {
        LLVMTypeRef ty = LLVMTypeOf(v);
        if (LLVMGetTypeKind(ty) != LLVMFloatTypeKind) {
            ERR(cg->fn_name,
                "non-f32 floating-point constant — only float supported");
            cg->had_error = 1;
            return 0;
        }
        LLVMBool lossy = 0;
        double   d     = LLVMConstRealGetDouble(v, &lossy);
        float    f     = (float)d;
        int32_t  bits;
        memcpy(&bits, &f, sizeof bits);
        uint8_t r = cg_alloc_reg(cg);
        if (cg->had_error) return 0;
        cg_emit_load_const32(cg, r, bits);
        return r;
    }

    /* `undef` (and `poison`, its strict subclass) appears when a value is
     * dynamically dead on some incoming PHI edge — clang emits it when
     * -O1 merges control flow that "can't" actually use the value. We
     * just hand back the dedicated zero register: the dynamic path that
     * would observe this value is dead by construction, and reusing
     * zero_reg avoids the per-use register churn a fresh MOVI would
     * cause (each call site for a phi input goes through cg_reg_for). */
    if (LLVMIsUndef(v)) {
        return cg->zero_reg;
    }

    if (cg->globals && LLVMIsAGlobalVariable(v)) {
        int idx = cg_globals_lookup(cg->globals, v);
        if (idx < 0) {
            ERR(cg->fn_name, "global '%s' not in layout", value_name(v));
            cg->had_error = 1;
            return 0;
        }
        uint8_t r = cg_alloc_reg(cg);
        if (cg->had_error) return 0;
        cg_emit_load_const32(cg, r, (int32_t)cg->globals->items[idx].offset);
        return r;
    }

    /* A Function value used as an operand (rather than as a call target)
     * means somebody is taking its address — for a function pointer or a
     * select/phi between two callees. In our world a function "address"
     * is its FUNCS-table index; user functions live at FUNCS[1..N]
     * (slot 0 is reserved as the null-function-pointer trap), so we
     * materialise `fidx + 1`. */
    if (LLVMIsAFunction(v)) {
        int fidx = cg_func_lookup(cg, v);
        if (fidx < 0) {
            ERR(cg->fn_name,
                "address taken of '%s': function has no definition in "
                "this module (extern is not supported)", value_name(v));
            cg->had_error = 1;
            return 0;
        }
        uint8_t r = cg_alloc_reg(cg);
        if (cg->had_error) return 0;
        cg_emit_load_const32(cg, r, fidx + 1);
        cg->funcs_referenced = 1;
        return r;
    }

    /* Constant expressions that address globals: a constant GEP
     * (`&global[k]`, `&struct.field`) or a pointer cast of one. clang -O1
     * emits these for stores/calls touching a global at a fixed offset.
     * GEP folds to base + static offset; bitcast/int<->ptr just reinterpret
     * the same 32-bit address, so we hand back the operand's register. */
    if (LLVMIsAConstantExpr(v)) {
        LLVMOpcode op = LLVMGetConstOpcode(v);
        if (op == LLVMBitCast || op == LLVMAddrSpaceCast ||
            op == LLVMIntToPtr || op == LLVMPtrToInt) {
            return cg_reg_for(cg, LLVMGetOperand(v, 0));
        }
        if (op == LLVMGetElementPtr) {
            long long off = 0;
            if (cg_const_gep_offset(cg, v, &off) != 0) {
                ERR(cg->fn_name, "unsupported constant GEP expression");
                cg->had_error = 1;
                return 0;
            }
            uint8_t base = cg_reg_for(cg, LLVMGetOperand(v, 0));
            if (cg->had_error) return 0;
            if (off == 0) return base;
            if (off < INT32_MIN || off > INT32_MAX) {
                ERR(cg->fn_name, "constant GEP offset %lld is wider than i32", off);
                cg->had_error = 1;
                return 0;
            }
            uint8_t off_r = cg_alloc_reg(cg);
            if (cg->had_error) return 0;
            cg_emit_load_const32(cg, off_r, (int32_t)off);
            uint8_t sum_r = cg_alloc_reg(cg);
            if (cg->had_error) return 0;
            cg_emit(cg, enc_r(CVM_OP_ADD, sum_r, base, off_r));
            return sum_r;
        }
        ERR(cg->fn_name, "unsupported constant expression (opcode %d)", (int)op);
        cg->had_error = 1;
        return 0;
    }

    ERR(cg->fn_name,
        "operand has no register assigned (use-before-def or unsupported value kind)");
    cg->had_error = 1;
    return 0;
}

/* --- block + fixup helpers ---------------------------------------------- */

static int cg_find_block(struct cg *cg, LLVMBasicBlockRef bb) {
    for (int i = 0; i < cg->block_count; ++i)
        if (cg->blocks[i] == bb) return i;
    return -1;
}

static void cg_register_block(struct cg *cg, LLVMBasicBlockRef bb) {
    if (cg->block_count == cg->block_cap) {
        cg->block_cap = cg->block_cap ? cg->block_cap * 2 : 8;
        cg->blocks = (LLVMBasicBlockRef *)realloc(cg->blocks,
                              cg->block_cap * sizeof(*cg->blocks));
        cg->block_offsets = (uint32_t *)realloc(cg->block_offsets,
                              cg->block_cap * sizeof(*cg->block_offsets));
        if (!cg->blocks || !cg->block_offsets) { perror("realloc"); exit(1); }
    }
    cg->blocks[cg->block_count] = bb;
    cg->block_offsets[cg->block_count] = 0;
    cg->block_count++;
}

static void cg_queue_fixup(struct cg *cg, uint32_t inst_index,
                           LLVMBasicBlockRef target, int shift, int bits)
{
    if (cg->fixup_count == cg->fixup_cap) {
        cg->fixup_cap = cg->fixup_cap ? cg->fixup_cap * 2 : 16;
        cg->fixups = (struct cg_fixup *)realloc(cg->fixups,
                                cg->fixup_cap * sizeof(*cg->fixups));
        if (!cg->fixups) { perror("realloc"); exit(1); }
    }
    cg->fixups[cg->fixup_count].inst_index = inst_index;
    cg->fixups[cg->fixup_count].target = target;
    cg->fixups[cg->fixup_count].shift = shift;
    cg->fixups[cg->fixup_count].bits = bits;
    cg->fixup_count++;
}

static int cg_queue_table_fixup(struct cg *cg, uint32_t data_off,
                                LLVMBasicBlockRef target)
{
    if (cg->table_fixup_count == cg->table_fixup_cap) {
        cg->table_fixup_cap = cg->table_fixup_cap ? cg->table_fixup_cap * 2 : 16;
        cg->table_fixups = (struct cg_table_fixup *)realloc(cg->table_fixups,
                                cg->table_fixup_cap * sizeof(*cg->table_fixups));
        if (!cg->table_fixups) { perror("realloc"); return 1; }
    }
    cg->table_fixups[cg->table_fixup_count].data_off = data_off;
    cg->table_fixups[cg->table_fixup_count].target   = target;
    cg->table_fixup_count++;
    return 0;
}

/* Resolve every queued switch jump-table entry. Runs after branch
 * relaxation so block_offsets are stable; writes the absolute
 * instruction index of each target block into globals.data_bytes at
 * the entry's reserved 4-byte slot, little-endian. */
static int cg_resolve_table_fixups(struct cg *cg) {
    for (int i = 0; i < cg->table_fixup_count; ++i) {
        struct cg_table_fixup *fx = &cg->table_fixups[i];
        int b = cg_find_block(cg, fx->target);
        if (b < 0) {
            ERR(cg->fn_name, "internal: table fixup target block not found");
            return 1;
        }
        uint32_t off = cg->block_offsets[b];
        uint8_t *p   = cg->globals->data_bytes + fx->data_off;
        p[0] = (uint8_t)(off);
        p[1] = (uint8_t)(off >>  8);
        p[2] = (uint8_t)(off >> 16);
        p[3] = (uint8_t)(off >> 24);
    }
    return 0;
}

/* Branch relaxation: convert any conditional branch (`BNE`/`BEQ` with imm8)
 * whose target is too far for the 8-bit immediate into a 3-instruction
 * trampoline that uses imm24 reach. The pattern is:
 *
 *     original:        BNE cond, zero, +K            ; imm8 to true_bb
 *                      JMP +M                        ; imm24 to false_bb
 *
 *     relaxed:         BEQ cond, zero, +1            ; skip next if false
 *                      JMP K'                        ; imm24 to true_bb
 *                      JMP M'                        ; imm24 to false_bb
 *
 * The opcode flips (BNE→BEQ or BEQ→BNE), the imm8 is a hard-coded +1, and
 * the original imm8 fixup migrates to the inserted JMP with bits=24.
 *
 * The pass runs to a fixed point because inserting a trampoline shifts
 * subsequent code by one instruction, which can push another in-range
 * branch out of range. Each iteration relaxes at least one fixup, so the
 * loop is bounded by the fixup count. The interpreter ABI is unchanged —
 * relaxation only re-shapes the emitted bytecode using existing opcodes. */
static int cg_relax_branches(struct cg *cg) {
    if (cg->fixup_count == 0) return 0;

    int *relaxed = (int *)calloc((size_t)cg->fixup_count, sizeof(int));
    if (!relaxed) { perror("calloc"); return 1; }

    for (int it = 0; it <= cg->fixup_count; ++it) {
        int changed = 0;
        for (int i = 0; i < cg->fixup_count; ++i) {
            if (cg->fixups[i].bits != 8 || relaxed[i]) continue;

            int target_b = cg_find_block(cg, cg->fixups[i].target);
            if (target_b < 0) {
                ERR(cg->fn_name, "internal: fixup target block not found");
                free(relaxed);
                return 1;
            }
            uint32_t orig_target    = cg->block_offsets[target_b];
            uint32_t orig_pc_after  = cg->fixups[i].inst_index + 1u;

            /* Effective offsets after already-decided relaxations: each
             * relaxed fixup inserts +1 instruction strictly after its own
             * inst_index, so positions ≥ inst_index+1 shift by +1. */
            int target_displ = 0, pc_displ = 0;
            for (int j = 0; j < cg->fixup_count; ++j) {
                if (!relaxed[j]) continue;
                if (cg->fixups[j].inst_index < orig_target)   target_displ++;
                if (cg->fixups[j].inst_index < orig_pc_after) pc_displ++;
            }
            int32_t rel = (int32_t)(orig_target + (uint32_t)target_displ)
                        - (int32_t)(orig_pc_after + (uint32_t)pc_displ);
            if (rel < -128 || rel > 127) {
                relaxed[i] = 1;
                changed = 1;
            }
        }
        if (!changed) break;
    }

    int relax_count = 0;
    for (int i = 0; i < cg->fixup_count; ++i)
        if (relaxed[i]) relax_count++;
    if (relax_count == 0) { free(relaxed); return 0; }

    /* Build the new code array. Each relaxed fixup contributes one extra
     * placeholder JMP right after its BEQ/BNE position. new_pos_of[op]
     * gives the new index for each old instruction. */
    uint32_t  old_size = cg->count;
    uint32_t  new_size = old_size + (uint32_t)relax_count;
    uint32_t *new_code   = (uint32_t *)malloc((size_t)new_size * sizeof(uint32_t));
    uint32_t *new_pos_of = (uint32_t *)malloc((size_t)old_size * sizeof(uint32_t));
    if (!new_code || !new_pos_of) {
        perror("malloc"); free(new_code); free(new_pos_of); free(relaxed);
        return 1;
    }

    uint32_t np = 0;
    for (uint32_t op = 0; op < old_size; ++op) {
        new_pos_of[op] = np;
        new_code[np++] = cg->code[op];
        for (int f = 0; f < cg->fixup_count; ++f) {
            if (relaxed[f] && cg->fixups[f].inst_index == op)
                new_code[np++] = enc_i24(CVM_OP_JMP, 0);
        }
    }

    /* Rewrite each relaxed BEQ/BNE: flip the opcode and set imm8 = +1.
     * Translator-emitted conditional brs only ever use BNE today (the
     * LLVMBr handler), but the BEQ branch is included for symmetry in
     * case future codegen paths emit a BEQ that needs relaxing. */
    for (int f = 0; f < cg->fixup_count; ++f) {
        if (!relaxed[f]) continue;
        uint32_t pos    = new_pos_of[cg->fixups[f].inst_index];
        uint32_t orig   = new_code[pos];
        uint8_t  op     = (uint8_t)(orig & 0xFFu);
        uint8_t  rs1    = (uint8_t)((orig >> 8)  & 0xFFu);
        uint8_t  rs2    = (uint8_t)((orig >> 16) & 0xFFu);
        uint8_t  new_op = (op == CVM_OP_BNE) ? CVM_OP_BEQ
                       : (op == CVM_OP_BEQ) ? CVM_OP_BNE
                       : op;
        new_code[pos] = enc_br(new_op, rs1, rs2, 1);
    }

    /* Update fixup positions and (for relaxed ones) bit-field metadata. */
    for (int f = 0; f < cg->fixup_count; ++f) {
        uint32_t old_pos = cg->fixups[f].inst_index;
        if (relaxed[f]) {
            cg->fixups[f].inst_index = new_pos_of[old_pos] + 1u;
            cg->fixups[f].shift      = 8;
            cg->fixups[f].bits       = 24;
        } else {
            cg->fixups[f].inst_index = new_pos_of[old_pos];
        }
    }

    /* Update block_offsets to their new positions. Empty blocks (offset
     * past the end) keep that property — clamp to new_size. */
    for (int b = 0; b < cg->block_count; ++b) {
        uint32_t orig = cg->block_offsets[b];
        cg->block_offsets[b] = orig < old_size ? new_pos_of[orig] : new_size;
    }

    free(cg->code);
    cg->code  = new_code;
    cg->count = new_size;
    cg->cap   = new_size;   /* malloc above sized to exactly new_size; cg_emit
                             * compares count == cap to decide when to grow,
                             * so leaving the old (larger) cap behind would
                             * let the next function's emit write past the
                             * fresh allocation. */
    free(new_pos_of);
    free(relaxed);
    return 0;
}

static int cg_resolve_fixups(struct cg *cg) {
    for (int i = 0; i < cg->fixup_count; ++i) {
        struct cg_fixup *fx = &cg->fixups[i];
        int b = cg_find_block(cg, fx->target);
        if (b < 0) {
            ERR(cg->fn_name, "internal: fixup target block not found");
            return 1;
        }
        int32_t target_off = (int32_t)cg->block_offsets[b];
        int32_t pc_after   = (int32_t)fx->inst_index + 1;
        int32_t rel        = target_off - pc_after;
        int32_t maxv       = (int32_t)(((uint32_t)1 << (fx->bits - 1)) - 1u);
        int32_t minv       = -maxv - 1;
        if (rel < minv || rel > maxv) {
            /* Defensive: cg_relax_branches should have lifted any
             * out-of-range imm8 fixup to imm24 before we get here. If
             * this fires, either relaxation didn't run or a new fixup
             * shape was introduced without relaxation support. */
            ERR(cg->fn_name,
                "internal: branch offset %d out of range [%d..%d] for "
                "%d-bit field after relaxation pass",
                rel, minv, maxv, fx->bits);
            return 1;
        }
        uint32_t mask = (fx->bits == 32) ? 0xFFFFFFFFu
                                         : ((uint32_t)1 << fx->bits) - 1u;
        cg->code[fx->inst_index] |= ((uint32_t)rel & mask) << fx->shift;
    }
    return 0;
}

/* For an edge from `from` to `to`, emit MOVs that copy each phi's incoming
 * value into the phi's pre-allocated register.
 *
 * Phi semantics are *parallel*: all incoming values are read at the moment
 * of the edge transition, then assigned simultaneously. Sequential MOV
 * emission breaks this when a destination is also another move's source
 * (loop back-edges with rotating state, e.g. a := b; b := a). To preserve
 * parallel semantics we detect any such conflict and round-trip through
 * temporary registers — wasteful, but always correct. */
static void cg_emit_phi_moves(struct cg *cg,
                              LLVMBasicBlockRef from, LLVMBasicBlockRef to)
{
    struct { uint8_t src; uint8_t dst; } moves[256];
    int nmoves = 0;

    for (LLVMValueRef inst = LLVMGetFirstInstruction(to);
         inst && LLVMGetInstructionOpcode(inst) == LLVMPHI;
         inst = LLVMGetNextInstruction(inst))
    {
        unsigned n = LLVMCountIncoming(inst);
        for (unsigned k = 0; k < n; ++k) {
            if (LLVMGetIncomingBlock(inst, k) != from) continue;
            LLVMValueRef src = LLVMGetIncomingValue(inst, k);
            uint8_t src_reg = cg_reg_for(cg, src);
            int phi_idx = cg_lookup(cg, inst);
            if (phi_idx < 0) {
                ERR(cg->fn_name, "internal: phi register not pre-allocated");
                cg->had_error = 1;
                return;
            }
            uint8_t phi_reg = cg->regs[phi_idx];
            if (src_reg == phi_reg) break;       /* no-op */
            if (nmoves >= (int)(sizeof moves / sizeof moves[0])) {
                ERR(cg->fn_name,
                    "internal: too many phi moves on a single edge");
                cg->had_error = 1;
                return;
            }
            moves[nmoves].src = src_reg;
            moves[nmoves].dst = phi_reg;
            nmoves++;
            break;
        }
    }

    /* Conflict iff any destination is also a source in the same batch. */
    int conflict = 0;
    for (int i = 0; i < nmoves && !conflict; ++i)
        for (int j = 0; j < nmoves && !conflict; ++j)
            if (i != j && moves[i].dst == moves[j].src) conflict = 1;

    if (!conflict) {
        for (int i = 0; i < nmoves; ++i)
            cg_emit(cg, enc_r(CVM_OP_MOV, moves[i].dst, moves[i].src, 0));
        return;
    }

    /* Round-trip through temps: read all sources, then write all destinations. */
    uint8_t temps[256];
    for (int i = 0; i < nmoves; ++i) {
        temps[i] = cg_alloc_reg(cg);
        if (cg->had_error) return;
        cg_emit(cg, enc_r(CVM_OP_MOV, temps[i], moves[i].src, 0));
    }
    for (int i = 0; i < nmoves; ++i)
        cg_emit(cg, enc_r(CVM_OP_MOV, moves[i].dst, temps[i], 0));
}

static int icmp_to_op(LLVMIntPredicate p, int *swap_out) {
    *swap_out = 0;
    switch (p) {
    case LLVMIntEQ:  return CVM_OP_CMP_EQ;
    case LLVMIntNE:  return CVM_OP_CMP_NE;
    case LLVMIntSLT: return CVM_OP_CMP_LT;
    case LLVMIntSLE: return CVM_OP_CMP_LE;
    case LLVMIntSGT: *swap_out = 1; return CVM_OP_CMP_LT;
    case LLVMIntSGE: *swap_out = 1; return CVM_OP_CMP_LE;
    case LLVMIntULT: return CVM_OP_CMP_LTU;
    case LLVMIntULE: return CVM_OP_CMP_LEU;
    case LLVMIntUGT: *swap_out = 1; return CVM_OP_CMP_LTU;
    case LLVMIntUGE: *swap_out = 1; return CVM_OP_CMP_LEU;
    default: return -1;
    }
}

/* --- prologue / epilogue / stack helpers --------------------------------- */

/* Materialise an arbitrary 32-bit immediate into `rd`. Single MOVI when
 * the value fits in int16; otherwise a MOVI(lo16) + MOVHI(hi16) pair. */
static void cg_emit_load_const32(struct cg *cg, uint8_t rd, int32_t imm) {
    if (imm >= INT16_MIN && imm <= INT16_MAX) {
        cg_emit(cg, enc_i16(CVM_OP_MOVI, rd, (int16_t)imm));
        return;
    }
    uint32_t u = (uint32_t)imm;
    cg_emit(cg, enc_i16(CVM_OP_MOVI,  rd, (int16_t)(uint16_t)(u & 0xFFFFu)));
    cg_emit(cg, enc_i16(CVM_OP_MOVHI, rd, (int16_t)(uint16_t)((u >> 16) & 0xFFFFu)));
}

/* Materialise `imm` into the scratch register (R254). Always succeeds. */
static int cg_movi_scratch(struct cg *cg, int32_t imm) {
    cg_emit_load_const32(cg, (uint8_t)CG_REG_SCRATCH, imm);
    return 0;
}

/* Compute R254 = R255 + offset. */
static int cg_addr_sp_plus(struct cg *cg, int32_t offset) {
    if (cg_movi_scratch(cg, offset)) return 1;
    cg_emit(cg, enc_r(CVM_OP_ADD, (uint8_t)CG_REG_SCRATCH,
                                  (uint8_t)CG_REG_SP,
                                  (uint8_t)CG_REG_SCRATCH));
    return 0;
}

/* SP -= n (n must be > 0 and fit in int16). Uses R254. */
static int cg_sp_sub(struct cg *cg, uint32_t n) {
    if (n == 0) return 0;
    if (cg_movi_scratch(cg, (int32_t)n)) return 1;
    cg_emit(cg, enc_r(CVM_OP_SUB, (uint8_t)CG_REG_SP,
                                  (uint8_t)CG_REG_SP,
                                  (uint8_t)CG_REG_SCRATCH));
    return 0;
}

/* SP += n. */
static int cg_sp_add(struct cg *cg, uint32_t n) {
    if (n == 0) return 0;
    if (cg_movi_scratch(cg, (int32_t)n)) return 1;
    cg_emit(cg, enc_r(CVM_OP_ADD, (uint8_t)CG_REG_SP,
                                  (uint8_t)CG_REG_SP,
                                  (uint8_t)CG_REG_SCRATCH));
    return 0;
}

/* Store R[src_reg] at address (SP + offset). Uses R254 as a temporary. */
static int cg_stw_sp_off(struct cg *cg, int32_t offset, uint8_t src_reg) {
    if (cg_addr_sp_plus(cg, offset)) return 1;
    cg_emit(cg, enc_r(CVM_OP_STW, 0, (uint8_t)CG_REG_SCRATCH, src_reg));
    return 0;
}

/* Load (SP + offset) into R[dst_reg]. Uses R254 as a temporary. */
static int cg_ldw_sp_off(struct cg *cg, uint8_t dst_reg, int32_t offset) {
    if (cg_addr_sp_plus(cg, offset)) return 1;
    cg_emit(cg, enc_r(CVM_OP_LDW, dst_reg, (uint8_t)CG_REG_SCRATCH, 0));
    return 0;
}

/* Walk the entry block, register every static-size alloca, and reject any
 * dynamic alloca or alloca outside the entry block. Sets cg->alloca_bytes. */
static int cg_collect_allocas(struct cg *cg, LLVMValueRef fn) {
    LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(fn);
    uint32_t cursor = 0;

    for (LLVMValueRef i = LLVMGetFirstInstruction(entry);
         i; i = LLVMGetNextInstruction(i))
    {
        if (LLVMGetInstructionOpcode(i) != LLVMAlloca) continue;
        LLVMValueRef cnt = LLVMGetOperand(i, 0);
        if (!LLVMIsAConstantInt(cnt)) {
            ERR(cg->fn_name, "dynamic alloca is not supported");
            cg->had_error = 1;
            return 1;
        }
        long long n = LLVMConstIntGetSExtValue(cnt);
        if (n < 0) {
            ERR(cg->fn_name, "alloca count is negative");
            cg->had_error = 1;
            return 1;
        }
        LLVMTypeRef ety = LLVMGetAllocatedType(i);
        uint32_t   esz = (uint32_t)LLVMABISizeOfType(cg->globals->td, ety);
        uint32_t   align = (uint32_t)LLVMABIAlignmentOfType(cg->globals->td, ety);
        if (align < 4) align = 4;
        cursor = (cursor + align - 1u) & ~(align - 1u);
        uint32_t size  = (uint32_t)n * esz;
        if (cg_alloca_append(cg, i, cursor) != 0) {
            cg->had_error = 1;
            return 1;
        }
        cursor += size;
    }
    /* round total alloca bytes to 4 so the spill area stays word-aligned. */
    cursor = (cursor + 3u) & ~3u;
    cg->alloca_bytes = cursor;

    /* Reject any alloca outside the entry block. */
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn);
         bb; bb = LLVMGetNextBasicBlock(bb))
    {
        if (bb == entry) continue;
        for (LLVMValueRef i = LLVMGetFirstInstruction(bb);
             i; i = LLVMGetNextInstruction(i))
        {
            if (LLVMGetInstructionOpcode(i) == LLVMAlloca) {
                ERR(cg->fn_name,
                    "alloca outside the entry block is not supported");
                cg->had_error = 1;
                return 1;
            }
        }
    }
    return 0;
}

/* True if every use of `v` is by a non-phi instruction in the same basic
 * block as `v`'s def. Such values are truly block-local: their lifetime
 * starts and ends inside that block, with no cross-edge escape (no phi
 * input role, no use from another block). Their register is safe to
 * recycle inside the block once the lexically-last use has passed. */
static int cg_value_is_block_local(LLVMValueRef v) {
    if (!LLVMIsAInstruction(v)) return 0;
    if (LLVMIsAPHINode(v))      return 0;        /* phi result escapes the def block */
    LLVMBasicBlockRef def_bb = LLVMGetInstructionParent(v);
    LLVMUseRef u = LLVMGetFirstUse(v);
    if (!u) return 0;                            /* dead — leave it to liveness/codegen */
    for (; u; u = LLVMGetNextUse(u)) {
        LLVMValueRef user = LLVMGetUser(u);
        if (!LLVMIsAInstruction(user))    return 0;
        if (LLVMIsAPHINode(user))         return 0;
        if (LLVMGetInstructionParent(user) != def_bb) return 0;
    }
    return 1;
}

/* True if no instruction strictly after `i` (in the same block) uses `v`.
 * Combined with `cg_value_is_block_local(v)`, this identifies the
 * lexically-last use of a block-local value — the point where its
 * register can be recycled. */
static int cg_is_last_use_in_block(LLVMValueRef i, LLVMValueRef v) {
    for (LLVMValueRef j = LLVMGetNextInstruction(i); j;
         j = LLVMGetNextInstruction(j))
    {
        unsigned nops = LLVMGetNumOperands(j);
        for (unsigned k = 0; k < nops; ++k) {
            if (LLVMGetOperand(j, k) == v) return 0;
        }
    }
    return 1;
}

static void cg_pre_alloc_function(struct cg *cg, LLVMValueRef fn) {
    /* R0..R7 are scratch for syscall argument passing. SSA values start at
     * R8 — the function prologue copies params from R0..R(N-1) into their
     * assigned high registers, so the syscall ABI can clobber R0..R7
     * freely without saving anything. */
    cg->next_reg = 8;
    cg->free_list_count = 0;

    unsigned np = LLVMCountParams(fn);
    for (unsigned i = 0; i < np; ++i) {
        LLVMValueRef p = LLVMGetParam(fn, i);
        cg_assign(cg, p, cg_alloc_reg(cg));   /* R8..R(8+N-1) */
    }
    /* zero_reg lives at the fixed CG_REG_ZERO (R253) across every
     * function — outside the spillable SSA range so callees can't
     * overwrite it. Each function's prologue still MOVIs R253 to 0
     * (see cg_function), which means after any call the caller's R253
     * is still 0 (the callee just re-set it on entry). */
    cg->zero_reg = (uint8_t)CG_REG_ZERO;

    /* Allocation strategy: each basic block runs an independent free
     * pool. A value-producing instruction draws from the pool first,
     * falling back to next_reg++. After processing each instruction,
     * any of its operands that are (a) defined in this same block,
     * (b) block-local per `cg_value_is_block_local`, and (c) not used
     * by any later instruction in this block, get their register
     * pushed back to the pool.
     *
     * Reuse is intentionally per-block, NOT function-wide: a block-
     * local value's register holds a "dead" payload after its in-block
     * last use, but if we let a *later* block reuse that register for
     * a cross-block value, a back-edge re-entering the original block
     * would have the local re-define the register and clobber the
     * cross-block value mid-life. Per-block scope avoids that hazard
     * without paying for full liveness intervals.
     *
     * The bit-level spill dataflow (cg_block_def_use et al.) tolerates
     * reuse natively: a reused register's def/use bits cover the union
     * of all values that ever lived in it, which is conservative but
     * always safe for the spill-across-call protocol. */
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn);
         bb; bb = LLVMGetNextBasicBlock(bb))
    {
        cg_register_block(cg, bb);
        cg->free_list_count = 0;          /* fresh pool per block */

        for (LLVMValueRef i = LLVMGetFirstInstruction(bb);
             i; i = LLVMGetNextInstruction(i))
        {
            int dst_reg = -1;
            LLVMTypeRef ty = LLVMTypeOf(i);
            if (LLVMGetTypeKind(ty) != LLVMVoidTypeKind) {
                dst_reg = cg_alloc_reg(cg);
                cg_assign(cg, i, (uint8_t)dst_reg);
            }

            /* After processing `i`, return any operand whose lifetime ends
             * here (block-local + no later in-block use) to the pool, so a
             * later instruction can recycle it.
             *
             * Crucially, skip an operand that shares `i`'s destination
             * register: cg_alloc_reg draws from the free pool, so `dst` can
             * legitimately land on the register of an operand that a *prior*
             * instruction already pooled (`MOV dst, dst` is a harmless
             * identity). But `dst` is live from here on — re-pooling that
             * register because the operand's last use was this instruction
             * would hand it to the next allocation while `dst` still needs
             * it, corrupting both. */
            unsigned nops = LLVMGetNumOperands(i);
            for (unsigned k = 0; k < nops; ++k) {
                LLVMValueRef v = LLVMGetOperand(i, k);
                if (!v) continue;
                if (!LLVMIsAInstruction(v)) continue;
                if (LLVMGetInstructionParent(v) != bb) continue;
                if (!cg_value_is_block_local(v)) continue;
                if (!cg_is_last_use_in_block(i, v)) continue;
                int v_idx = cg_lookup(cg, v);
                if (v_idx < 0) continue;
                if ((int)cg->regs[v_idx] == dst_reg) continue;   /* dst owns it now */
                cg_free_reg(cg, cg->regs[v_idx]);
            }
        }
    }

    /* The emit phase materialises constants/globals as transient regs
     * via cg_reg_for → cg_alloc_reg. Clearing the pool here keeps that
     * path strictly above ssa_reg_high (the pre-alloc high-water mark),
     * which the spill analysis treats as never-spilled scratch. */
    cg->free_list_count = 0;
}

/* --- Liveness analysis --------------------------------------------------- */

/* Map an LLVM value (instruction or function parameter) to its bit index in
 * the spill bitset, or -1 if the value isn't held in a pre-allocated SSA
 * register. Constants, globals, function values, basic-block targets, and
 * transient temps above ssa_reg_high all return -1. */
static int cg_spill_bit_of(struct cg *cg, LLVMValueRef v) {
    if (!v) return -1;
    if (!LLVMIsAInstruction(v) && !LLVMIsAArgument(v)) return -1;
    int idx = cg_lookup(cg, v);
    if (idx < 0) return -1;
    uint8_t r = cg->regs[idx];
    if (r < 8u || (int)r >= cg->ssa_reg_high) return -1;
    return (int)r - 8;
}

/* Compute def[bb], use[bb] (upward-exposed uses), and phi_def[bb] for one
 * basic block. Phi instructions are skipped during the upward-use walk
 * (their source operands belong to the predecessor edges, not to this
 * block); phi result registers are subtracted from `use` at the end so a
 * non-phi instruction that reads a phi result doesn't make the phi result
 * appear upward-exposed. */
static void cg_block_def_use(struct cg *cg, LLVMBasicBlockRef bb,
                             int is_entry, LLVMValueRef fn,
                             cg_bits *def_out, cg_bits *use_out,
                             cg_bits *phi_def_out)
{
    cg_bits_clear(def_out);
    cg_bits_clear(use_out);
    cg_bits_clear(phi_def_out);

    /* Collect every instruction-defined SSA register, plus the phi subset. */
    for (LLVMValueRef i = LLVMGetFirstInstruction(bb);
         i; i = LLVMGetNextInstruction(i))
    {
        int b = cg_spill_bit_of(cg, i);
        if (b < 0) continue;
        cg_bits_set(def_out, (unsigned)b);
        if (LLVMGetInstructionOpcode(i) == LLVMPHI)
            cg_bits_set(phi_def_out, (unsigned)b);
    }

    /* In the entry block, parameters are conceptually defined at the top
     * (the prologue copies R0..R7 into their high SSA homes). Marking them
     * as defs prevents them from being treated as upward-exposed uses. */
    if (is_entry) {
        unsigned np = LLVMCountParams(fn);
        for (unsigned p = 0; p < np; ++p) {
            int b = cg_spill_bit_of(cg, LLVMGetParam(fn, p));
            if (b >= 0) cg_bits_set(def_out, (unsigned)b);
        }
    }

    /* Backward walk over non-phi instructions to derive upward-exposed uses. */
    cg_bits live; cg_bits_clear(&live);
    for (LLVMValueRef i = LLVMGetLastInstruction(bb);
         i; i = LLVMGetPreviousInstruction(i))
    {
        if (LLVMGetInstructionOpcode(i) == LLVMPHI) continue;

        int db = cg_spill_bit_of(cg, i);
        if (db >= 0) cg_bits_clear_bit(&live, (unsigned)db);

        unsigned no = LLVMGetNumOperands(i);
        for (unsigned k = 0; k < no; ++k) {
            int ob = cg_spill_bit_of(cg, LLVMGetOperand(i, k));
            if (ob >= 0) cg_bits_set(&live, (unsigned)ob);
        }
    }
    /* Phi results are defined at the very top of the block (logically before
     * any non-phi inst), so any non-phi use we tracked is satisfied locally. */
    cg_bits_andnot(&live, phi_def_out);
    *use_out = live;
}

/* Look up a block's index in cg->blocks (linear scan; block counts are tiny). */
static int cg_block_index(struct cg *cg, LLVMBasicBlockRef bb) {
    for (int i = 0; i < cg->block_count; ++i)
        if (cg->blocks[i] == bb) return i;
    return -1;
}

/* OR the set of phi-input source registers from edge P→S into `out`. For
 * each phi at the top of S, find the incoming pair whose predecessor block
 * is P and add the source value's spill bit. */
static void cg_or_phi_use_from_edge(struct cg *cg,
                                    LLVMBasicBlockRef P, LLVMBasicBlockRef S,
                                    cg_bits *out)
{
    for (LLVMValueRef phi = LLVMGetFirstInstruction(S); phi;
         phi = LLVMGetNextInstruction(phi))
    {
        if (LLVMGetInstructionOpcode(phi) != LLVMPHI) break;
        unsigned ni = LLVMCountIncoming(phi);
        for (unsigned k = 0; k < ni; ++k) {
            if (LLVMGetIncomingBlock(phi, k) != P) continue;
            int eb = cg_spill_bit_of(cg, LLVMGetIncomingValue(phi, k));
            if (eb >= 0) cg_bits_set(out, (unsigned)eb);
            break;
        }
    }
}

/* Standard fixed-point liveness with phi handling:
 *
 *   live_in[bb]  = use[bb] ∪ (live_out[bb] − def[bb])
 *   live_out[P]  = ∪_{S ∈ succ(P)} ((live_in[S] − phi_def[S]) ∪ phi_use(P,S))
 *
 * Iterates blocks in reverse order (cheap proxy for reverse postorder).
 * Block counts are small enough that the naive iteration converges in a
 * handful of passes. */
static void cg_compute_liveness(struct cg *cg, LLVMValueRef fn) {
    int n = cg->block_count;
    if (n == 0) return;

    if (n > cg->live_cap) {
        cg->live_cap = n;
        cg->bb_live_in  = (cg_bits *)realloc(cg->bb_live_in,
                                             (size_t)n * sizeof(cg_bits));
        cg->bb_live_out = (cg_bits *)realloc(cg->bb_live_out,
                                             (size_t)n * sizeof(cg_bits));
        if (!cg->bb_live_in || !cg->bb_live_out) {
            perror("realloc"); exit(1);
        }
    }

    cg_bits *def     = (cg_bits *)malloc((size_t)n * sizeof(cg_bits));
    cg_bits *use     = (cg_bits *)malloc((size_t)n * sizeof(cg_bits));
    cg_bits *phi_def = (cg_bits *)malloc((size_t)n * sizeof(cg_bits));
    if (!def || !use || !phi_def) { perror("malloc"); exit(1); }

    LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(fn);
    for (int b = 0; b < n; ++b) {
        cg_block_def_use(cg, cg->blocks[b], cg->blocks[b] == entry, fn,
                         &def[b], &use[b], &phi_def[b]);
        cg_bits_clear(&cg->bb_live_in[b]);
        cg_bits_clear(&cg->bb_live_out[b]);
    }

    int changed = 1;
    while (changed) {
        changed = 0;
        for (int b = n - 1; b >= 0; --b) {
            LLVMBasicBlockRef bb = cg->blocks[b];
            cg_bits new_out; cg_bits_clear(&new_out);

            LLVMValueRef term = LLVMGetBasicBlockTerminator(bb);
            unsigned ns = term ? LLVMGetNumSuccessors(term) : 0;
            for (unsigned s = 0; s < ns; ++s) {
                LLVMBasicBlockRef succ = LLVMGetSuccessor(term, s);
                int si = cg_block_index(cg, succ);
                if (si < 0) continue;

                cg_bits tmp = cg->bb_live_in[si];
                cg_bits_andnot(&tmp, &phi_def[si]);
                cg_bits_or(&new_out, &tmp);
                cg_or_phi_use_from_edge(cg, bb, succ, &new_out);
            }

            cg_bits new_in = use[b];
            cg_bits tmp_in = new_out;
            cg_bits_andnot(&tmp_in, &def[b]);
            cg_bits_or(&new_in, &tmp_in);

            if (!cg_bits_eq(&new_in, &cg->bb_live_in[b])) {
                changed = 1;
                cg->bb_live_in[b] = new_in;
            }
            if (!cg_bits_eq(&new_out, &cg->bb_live_out[b])) {
                changed = 1;
                cg->bb_live_out[b] = new_out;
            }
        }
    }

    free(def); free(use); free(phi_def);
}

/* Walk every block backward and snapshot, for each LLVMCall instruction,
 * the set of spillable SSA registers live at the program point immediately
 * after the call. The CALL handler in cg_function consults this table to
 * spill only what the callee would clobber. */
static void cg_compute_call_liveouts(struct cg *cg) {
    cg->call_live_count = 0;
    for (int b = 0; b < cg->block_count; ++b) {
        LLVMBasicBlockRef bb = cg->blocks[b];
        cg_bits live = cg->bb_live_out[b];
        for (LLVMValueRef i = LLVMGetLastInstruction(bb); i;
             i = LLVMGetPreviousInstruction(i))
        {
            LLVMOpcode op = LLVMGetInstructionOpcode(i);
            if (op == LLVMPHI) continue;

            /* Snapshot before applying i's effects: `live` here is the set
             * of registers live at the program point AFTER i has executed. */
            if (op == LLVMCall) {
                if (cg->call_live_count == cg->call_live_cap) {
                    cg->call_live_cap = cg->call_live_cap
                                        ? cg->call_live_cap * 2 : 16;
                    cg->call_lives = (struct cg_call_live *)realloc(
                        cg->call_lives,
                        (size_t)cg->call_live_cap * sizeof(*cg->call_lives));
                    if (!cg->call_lives) { perror("realloc"); exit(1); }
                }
                cg->call_lives[cg->call_live_count].inst       = i;
                cg->call_lives[cg->call_live_count].live_after = live;
                cg->call_live_count++;
            }

            int db = cg_spill_bit_of(cg, i);
            if (db >= 0) cg_bits_clear_bit(&live, (unsigned)db);
            unsigned no = LLVMGetNumOperands(i);
            for (unsigned k = 0; k < no; ++k) {
                int ob = cg_spill_bit_of(cg, LLVMGetOperand(i, k));
                if (ob >= 0) cg_bits_set(&live, (unsigned)ob);
            }
        }
    }
}

static const cg_bits *cg_lookup_call_live(struct cg *cg, LLVMValueRef call) {
    for (int i = 0; i < cg->call_live_count; ++i)
        if (cg->call_lives[i].inst == call)
            return &cg->call_lives[i].live_after;
    return NULL;
}

static int cg_function(struct cg *cg, LLVMValueRef fn, int func_idx) {
    cg_reset_function_state(cg);
    cg->fn_name = value_name(fn);

    if (cg_collect_allocas(cg, fn) != 0) return 1;

    cg_pre_alloc_function(cg, fn);
    if (cg->had_error) return 1;

    /* Snapshot the SSA register high-water mark. cg_reg_for grows next_reg
     * during emission whenever it materialises a constant; those transient
     * registers are dead after their parent instruction and must not be
     * counted in spill_bytes. */
    cg->ssa_reg_high = cg->next_reg;

    /* Liveness analysis: lets the LLVMCall handler spill only the SSA
     * registers actually live across each call site. */
    cg_compute_liveness(cg, fn);
    cg_compute_call_liveouts(cg);

    /* Compact spill layout. The naïve mapping is one slot per
     * pre-allocated SSA register (R8..R(ssa_reg_high-1)); the
     * compacted version only allocates a slot when the register is
     * live across at least one call. For SSA values that never cross
     * a call (most temporaries in a leaf function), no slot is
     * needed, and frame_bytes shrinks accordingly.
     *
     * Layout (low → high): [alloca area | spill area]. The spill area
     * holds spill_slot_count i32 slots; slot_of[bit] gives the compact
     * index for SSA reg (bit + 8), or 0xFF if the reg never crossed a
     * call (no slot reserved — the LLVMCall handler must not write to
     * such a register). */
    int spill_count = cg->ssa_reg_high - 8;
    if (spill_count > (int)sizeof(cg->slot_of)) {
        ERR(cg->fn_name,
            "internal: ssa_reg_high - 8 = %d exceeds slot_of[] capacity (%zu)",
            spill_count, sizeof(cg->slot_of));
        return 1;
    }
    cg_bits_clear(&cg->ever_spilled);
    for (int i = 0; i < cg->call_live_count; ++i)
        cg_bits_or(&cg->ever_spilled, &cg->call_lives[i].live_after);
    memset(cg->slot_of, 0xFF, sizeof cg->slot_of);
    cg->spill_slot_count = 0;
    for (int k = 0; k < spill_count; ++k) {
        if (cg_bits_test(&cg->ever_spilled, (unsigned)k))
            cg->slot_of[k] = (uint8_t)cg->spill_slot_count++;
    }
    cg->spill_bytes = (uint32_t)cg->spill_slot_count * 4u;
    cg->frame_bytes = cg->alloca_bytes + cg->spill_bytes;
    cg->funcs[func_idx].frame_size = cg->frame_bytes;

    /* Prologue: SUB SP, SP, frame. */
    if (cg_sp_sub(cg, cg->frame_bytes)) return 1;

    /* Copy first-8 params from the calling-convention regs into their high
     * SSA homes, and load any stacked params (>=9th) from caller's frame.
     *
     * Stacked args sit just above the return PC the callee's CALL pushed:
     *   addr(stacked_arg_i) = SP_after_prologue + frame + 4 + i*4
     * where the +4 accounts for the saved return PC. */
    unsigned n_params = LLVMCountParams(fn);
    /* Variadic functions take ALL their named params on the stack (the i386
     * vararg ABI), so the unnamed args follow them contiguously in memory and
     * va_start can walk them. Non-variadic functions use the normal ABI:
     * first 8 params in the calling-convention regs, the 9th onward stacked. */
    int fn_is_vararg = LLVMIsFunctionVarArg(LLVMGlobalGetValueType(fn));
    for (unsigned p = 0; p < n_params; ++p) {
        LLVMValueRef pv  = LLVMGetParam(fn, p);
        uint8_t      dst = cg->regs[cg_lookup(cg, pv)];
        if (!fn_is_vararg && p < 8) {
            if (dst != p)
                cg_emit(cg, enc_r(CVM_OP_MOV, dst, (uint8_t)p, 0));
        } else {
            unsigned slot = fn_is_vararg ? p : (p - 8);
            int32_t off = (int32_t)cg->frame_bytes + 4 + (int32_t)slot * 4;
            if (cg_ldw_sp_off(cg, dst, off)) return 1;
        }
    }
    cg_emit(cg, enc_i16(CVM_OP_MOVI, cg->zero_reg, 0));

    /* Materialise alloca pointers: each %p = SP + alloca_offset. SP doesn't
     * move within the body except inside CALL sequences, but those sequences
     * never read alloca pointers, so the snapshotted absolute addresses
     * remain valid for every subsequent use. */
    for (int k = 0; k < cg->alloca_count; ++k) {
        LLVMValueRef av = cg->allocas[k].value;
        int idx = cg_lookup(cg, av);
        if (idx < 0) {
            ERR(cg->fn_name, "internal: alloca register not pre-allocated");
            cg->had_error = 1;
            return 1;
        }
        uint8_t dst = cg->regs[idx];
        int32_t off = (int32_t)cg->allocas[k].offset;
        if (cg_movi_scratch(cg, off)) return 1;
        cg_emit(cg, enc_r(CVM_OP_ADD, dst,
                                      (uint8_t)CG_REG_SP,
                                      (uint8_t)CG_REG_SCRATCH));
    }

    for (int b = 0; b < cg->block_count && !cg->had_error; ++b) {
        LLVMBasicBlockRef bb = cg->blocks[b];
        cg->block_offsets[b] = cg->count;
        cg->cur_block = bb;

        for (LLVMValueRef i = LLVMGetFirstInstruction(bb);
             i && !cg->had_error; i = LLVMGetNextInstruction(i))
        {
            LLVMOpcode op = LLVMGetInstructionOpcode(i);

            /* Recycle the transient scratch region (R[ssa_reg_high..]) at the
             * start of every instruction. cg_reg_for materialises constants,
             * globals and constant-expressions into fresh registers above
             * ssa_reg_high and never caches them across instructions, and an
             * instruction's SSA *result* always lands in a pre-allocated
             * register (< ssa_reg_high). So those scratch registers are dead
             * once the instruction that produced them finishes — reusing them
             * each instruction keeps next_reg from growing monotonically and
             * exhausting the file in constant-heavy functions (e.g. a frame
             * that issues many syscalls with immediate arguments). */
            cg->next_reg = cg->ssa_reg_high;

            switch (op) {
            case LLVMPHI:
                /* phis are eliminated by emit_phi_moves on predecessor edges. */
                break;

            case LLVMAdd:  case LLVMSub:  case LLVMMul:
            case LLVMSDiv: case LLVMUDiv: case LLVMSRem: case LLVMURem:
            case LLVMShl:  case LLVMLShr: case LLVMAShr:
            case LLVMAnd:  case LLVMOr:   case LLVMXor: {
                uint8_t lhs = cg_reg_for(cg, LLVMGetOperand(i, 0));
                uint8_t rhs = cg_reg_for(cg, LLVMGetOperand(i, 1));
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                uint8_t cv;
                switch (op) {
                case LLVMAdd:  cv = CVM_OP_ADD;  break;
                case LLVMSub:  cv = CVM_OP_SUB;  break;
                case LLVMMul:  cv = CVM_OP_MUL;  break;
                case LLVMSDiv: cv = CVM_OP_DIV;  break;
                case LLVMUDiv: cv = CVM_OP_DIVU; break;
                case LLVMSRem: cv = CVM_OP_MOD;  break;
                case LLVMURem: cv = CVM_OP_MODU; break;
                case LLVMShl:  cv = CVM_OP_SHL;  break;
                case LLVMLShr: cv = CVM_OP_SHR;  break;
                case LLVMAShr: cv = CVM_OP_SAR;  break;
                case LLVMAnd:  cv = CVM_OP_AND;  break;
                case LLVMOr:   cv = CVM_OP_OR;   break;
                case LLVMXor:  cv = CVM_OP_XOR;  break;
                default:       cv = CVM_OP_ADD;  break; /* unreachable */
                }
                cg_emit(cg, enc_r(cv, dst, lhs, rhs));
                break;
            }

            case LLVMICmp: {
                LLVMIntPredicate p = LLVMGetICmpPredicate(i);
                int swap = 0;
                int op2 = icmp_to_op(p, &swap);
                if (op2 < 0) {
                    ERR(cg->fn_name, "unsupported icmp predicate");
                    cg->had_error = 1;
                    break;
                }
                uint8_t lhs = cg_reg_for(cg, LLVMGetOperand(i, 0));
                uint8_t rhs = cg_reg_for(cg, LLVMGetOperand(i, 1));
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                if (swap) cg_emit(cg, enc_r((uint8_t)op2, dst, rhs, lhs));
                else      cg_emit(cg, enc_r((uint8_t)op2, dst, lhs, rhs));
                break;
            }

            /* f32 binops. Mirror the integer-binop pattern; the ISA gives
             * us a 1:1 opcode for each LLVM op so no synthesis is needed. */
            case LLVMFAdd: case LLVMFSub: case LLVMFMul: case LLVMFDiv: {
                uint8_t lhs = cg_reg_for(cg, LLVMGetOperand(i, 0));
                uint8_t rhs = cg_reg_for(cg, LLVMGetOperand(i, 1));
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                uint8_t cv;
                switch (op) {
                case LLVMFAdd: cv = CVM_OP_FADD; break;
                case LLVMFSub: cv = CVM_OP_FSUB; break;
                case LLVMFMul: cv = CVM_OP_FMUL; break;
                case LLVMFDiv: cv = CVM_OP_FDIV; break;
                default:       cv = CVM_OP_FADD; break; /* unreachable */
                }
                cg_emit(cg, enc_r(cv, dst, lhs, rhs));
                break;
            }

            case LLVMFNeg: {
                uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                cg_emit(cg, enc_r(CVM_OP_FNEG, dst, src, 0));
                break;
            }

            case LLVMFCmp: {
                /* LLVM has 14 FP predicates split into ordered (`O*`,
                 * NaN→false) and unordered (`U*`, NaN→true) flavours.
                 * The natural C operators map cleanly to a small subset:
                 *   ==  → OEQ (FCMP_EQ)
                 *   !=  → UNE (FCMP_NE)        — matches IEEE C semantics
                 *   <   → OLT (FCMP_LT)
                 *   <=  → OLE (FCMP_LE)
                 *   >   → OGT (FCMP_LT, swap)
                 *   >=  → OGE (FCMP_LE, swap)
                 * Plus two NaN-only predicates that clang -O1 InstCombine
                 * loves to introduce by folding `x == x` and isnan-style
                 * bit-pattern checks:
                 *   ord(a,b) ↔ both not NaN  → FCMP_EQ a,a AND FCMP_EQ b,b
                 *   uno(a,b) ↔ either is NaN → FCMP_NE a,a OR  FCMP_NE b,b
                 * Lowered to three instructions each since IEEE NaN
                 * compares unequal to itself. The remaining six predicates
                 * (ONE, UEQ, ULT, ULE, UGT, UGE) almost never appear from
                 * straight C code and are rejected. */
                LLVMRealPredicate p = LLVMGetFCmpPredicate(i);
                int op2 = -1, swap = 0;
                int compound_kind = 0;       /* 0=normal, 1=ord, 2=uno */
                switch (p) {
                case LLVMRealOEQ: op2 = CVM_OP_FCMP_EQ;             break;
                case LLVMRealUNE: op2 = CVM_OP_FCMP_NE;             break;
                case LLVMRealOLT: op2 = CVM_OP_FCMP_LT;             break;
                case LLVMRealOLE: op2 = CVM_OP_FCMP_LE;             break;
                case LLVMRealOGT: op2 = CVM_OP_FCMP_LT; swap = 1;   break;
                case LLVMRealOGE: op2 = CVM_OP_FCMP_LE; swap = 1;   break;
                case LLVMRealORD: compound_kind = 1;                break;
                case LLVMRealUNO: compound_kind = 2;                break;
                default: break;
                }
                if (op2 < 0 && compound_kind == 0) {
                    ERR(cg->fn_name,
                        "fcmp predicate %d not in the supported subset "
                        "(use ==, !=, <, <=, >, >=)", (int)p);
                    cg->had_error = 1;
                    break;
                }
                uint8_t lhs = cg_reg_for(cg, LLVMGetOperand(i, 0));
                uint8_t rhs = cg_reg_for(cg, LLVMGetOperand(i, 1));
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                if (compound_kind == 1) {
                    /* ord(a,b) = (a==a) & (b==b) */
                    uint8_t t1 = cg_alloc_reg(cg);
                    uint8_t t2 = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_FCMP_EQ, t1,  lhs, lhs));
                    cg_emit(cg, enc_r(CVM_OP_FCMP_EQ, t2,  rhs, rhs));
                    cg_emit(cg, enc_r(CVM_OP_AND,     dst, t1,  t2 ));
                } else if (compound_kind == 2) {
                    /* uno(a,b) = (a!=a) | (b!=b) */
                    uint8_t t1 = cg_alloc_reg(cg);
                    uint8_t t2 = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_FCMP_NE, t1,  lhs, lhs));
                    cg_emit(cg, enc_r(CVM_OP_FCMP_NE, t2,  rhs, rhs));
                    cg_emit(cg, enc_r(CVM_OP_OR,      dst, t1,  t2 ));
                } else if (swap) {
                    cg_emit(cg, enc_r((uint8_t)op2, dst, rhs, lhs));
                } else {
                    cg_emit(cg, enc_r((uint8_t)op2, dst, lhs, rhs));
                }
                break;
            }

            case LLVMSIToFP: case LLVMUIToFP:
            case LLVMFPToSI: case LLVMFPToUI: {
                uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                uint8_t cv;
                switch (op) {
                case LLVMSIToFP: cv = CVM_OP_I2F_S; break;
                case LLVMUIToFP: cv = CVM_OP_I2F_U; break;
                case LLVMFPToSI: cv = CVM_OP_F2I_S; break;
                case LLVMFPToUI: cv = CVM_OP_F2I_U; break;
                default:         cv = CVM_OP_I2F_S; break; /* unreachable */
                }
                cg_emit(cg, enc_r(cv, dst, src, 0));
                break;
            }

            case LLVMSelect: {
                uint8_t cond = cg_reg_for(cg, LLVMGetOperand(i, 0));
                uint8_t a    = cg_reg_for(cg, LLVMGetOperand(i, 1));
                uint8_t b2   = cg_reg_for(cg, LLVMGetOperand(i, 2));
                uint8_t dst  = cg->regs[cg_lookup(cg, i)];
                /* Inline branch pattern, 4 instructions:
                 *   BEQ cond, zero, +2   ; if false, skip true-MOV
                 *   MOV dst, a           ; true value
                 *   JMP +1               ; over the false-MOV
                 *   MOV dst, b           ; false value */
                cg_emit(cg, enc_br(CVM_OP_BEQ, cond, cg->zero_reg, 2));
                cg_emit(cg, enc_r (CVM_OP_MOV, dst, a, 0));
                cg_emit(cg, enc_i24(CVM_OP_JMP, 1));
                cg_emit(cg, enc_r (CVM_OP_MOV, dst, b2, 0));
                break;
            }

            case LLVMBr: {
                if (LLVMIsConditional(i)) {
                    uint8_t cond_reg = cg_reg_for(cg, LLVMGetCondition(i));
                    LLVMBasicBlockRef true_bb  = LLVMGetSuccessor(i, 0);
                    LLVMBasicBlockRef false_bb = LLVMGetSuccessor(i, 1);
                    cg_emit_phi_moves(cg, cg->cur_block, true_bb);
                    cg_emit_phi_moves(cg, cg->cur_block, false_bb);
                    cg_queue_fixup(cg, cg->count, true_bb, 24, 8);
                    cg_emit(cg, enc_br(CVM_OP_BNE, cond_reg, cg->zero_reg, 0));
                    cg_queue_fixup(cg, cg->count, false_bb, 8, 24);
                    cg_emit(cg, enc_i24(CVM_OP_JMP, 0));
                } else {
                    LLVMBasicBlockRef target = LLVMGetSuccessor(i, 0);
                    cg_emit_phi_moves(cg, cg->cur_block, target);
                    cg_queue_fixup(cg, cg->count, target, 8, 24);
                    cg_emit(cg, enc_i24(CVM_OP_JMP, 0));
                }
                break;
            }

            case LLVMSwitch: {
                /* Two lowerings:
                 *   - **Chain**: for each case k, `CMP_EQ tmp, cond, ck;
                 *     BNE tmp, zero, case_bb_k`, then `JMP default_bb`.
                 *     O(N) dispatch. Always emitted for sparse / few-case
                 *     switches.
                 *   - **Table**: a u32[N_range] table in DATA holding
                 *     absolute instruction indices, one per slot in
                 *     [case_min, case_max]. Dispatch is bounds-check +
                 *     LDW + JMPR. O(1). Emitted when N_cases ≥ 4 AND
                 *     density ≥ 0.5 (i.e. range ≤ 2 × N_cases).
                 *
                 * Phi moves for every successor (default + each case) are
                 * emitted up front, before any test or table dispatch.
                 * Each successor's phi-result registers are unique to
                 * that successor (SSA gives every value its own pre-
                 * allocated register), so the moves don't interfere with
                 * each other and the dispatch sequence (which only
                 * touches transient temps above ssa_reg_high) doesn't
                 * disturb the phi values.
                 *
                 * `LLVMGetCondition` is only valid for `LLVMBranchInst`;
                 * on a switch it returns NULL. The condition is at
                 * operand 0. Case constants reach us only through the
                 * C++ helper SwitchInst::CaseHandle::getCaseValue() —
                 * the LLVM-C operand list exposes successor blocks
                 * (cond + default + case_bbs) but hides the constants,
                 * and the upstream `LLVMGetSwitchCaseValue` wrapper
                 * landed too late to rely on. Our
                 * `cvm_llvm_get_switch_case_value` shim bridges to the
                 * C++ method; see llvm_c_compat.cpp. */
                LLVMValueRef       cond_v     = LLVMGetOperand(i, 0);
                LLVMBasicBlockRef  default_bb = LLVMGetSwitchDefaultDest(i);
                uint8_t            cond_reg   = cg_reg_for(cg, cond_v);
                if (cg->had_error) break;

                /* `cond_reg` is whatever the predecessor produced, which
                 * for an `iN` cond (N < 32) is now zero-extended to N bits
                 * by the Trunc lowering. Case constants must be read in
                 * the same width-mod basis — see Trunc/chain-form notes —
                 * or the table form's `SUB off, cond, lo` would sign-mix
                 * a 0xFF cond with a 0xFFFFFFFE lo and the CMP_LTU would
                 * miss every case. Hoisted here so both lowerings agree. */
                LLVMTypeRef cond_ty = LLVMTypeOf(cond_v);
                unsigned    cond_w  =
                    (LLVMGetTypeKind(cond_ty) == LLVMIntegerTypeKind)
                        ? LLVMGetIntTypeWidth(cond_ty) : 32;
                uint64_t    cond_mask =
                    (cond_w >= 64) ? ~0ULL : ((1ULL << cond_w) - 1ULL);

                unsigned n_succ = LLVMGetNumSuccessors(i);
                /* n_succ counts the default plus each case block; LLVM
                 * guarantees ≥ 1 (the default). Switches with zero cases
                 * are degenerate but legal — they reduce to a JMP. */
                cg_emit_phi_moves(cg, cg->cur_block, default_bb);
                for (unsigned k = 1; k < n_succ; ++k)
                    cg_emit_phi_moves(cg, cg->cur_block, LLVMGetSuccessor(i, k));
                if (cg->had_error) break;

                unsigned n_cases = n_succ - 1;

                /* Decide chain vs table. Both heuristic conditions
                 * (N_cases ≥ 4, range ≤ 2 × N_cases) must hold; with
                 * range ≤ 2 × N_cases the table wastes at most one
                 * default-pointing slot per real case. */
                int     use_table = 0;
                int32_t lo = 0, hi = 0;
                uint32_t n_range = 0;
                if (n_cases >= 4) {
                    int valid = 1;
                    for (unsigned k = 1; k < n_succ; ++k) {
                        LLVMValueRef cv = cvm_llvm_get_switch_case_value(i, k);
                        if (!cv || !LLVMIsAConstantInt(cv)) { valid = 0; break; }
                        uint64_t zv = LLVMConstIntGetZExtValue(cv) & cond_mask;
                        if (zv > (uint64_t)UINT32_MAX) { valid = 0; break; }
                        int32_t iv = (int32_t)(uint32_t)zv;
                        if (k == 1) { lo = hi = iv; }
                        else { if (iv < lo) lo = iv; if (iv > hi) hi = iv; }
                    }
                    if (valid) {
                        int64_t range = (int64_t)hi - (int64_t)lo + 1;
                        if (range > 0 && range <= 2 * (int64_t)n_cases) {
                            n_range   = (uint32_t)range;
                            use_table = 1;
                        }
                    }
                }

                if (use_table) {
                    /* Reserve N_range × 4 bytes in DATA and queue a table
                     * fixup per slot. Slots default to default_bb;
                     * present cases overwrite their slot's target. */
                    uint32_t table_off = cg->globals->data_size;
                    cg_data_reserve(cg->globals, table_off + n_range * 4u);
                    cg->globals->data_size = table_off + n_range * 4u;

                    LLVMBasicBlockRef *targets =
                        (LLVMBasicBlockRef *)malloc(n_range * sizeof(*targets));
                    if (!targets) {
                        ERR(cg->fn_name, "malloc"); cg->had_error = 1; break;
                    }
                    for (uint32_t k = 0; k < n_range; ++k) targets[k] = default_bb;
                    for (unsigned k = 1; k < n_succ; ++k) {
                        LLVMValueRef cv = cvm_llvm_get_switch_case_value(i, k);
                        int32_t  v   = (int32_t)(uint32_t)
                                       (LLVMConstIntGetZExtValue(cv) & cond_mask);
                        uint32_t idx = (uint32_t)(v - lo);
                        targets[idx] = LLVMGetSuccessor(i, k);
                    }
                    for (uint32_t k = 0; k < n_range; ++k) {
                        if (cg_queue_table_fixup(cg, table_off + k * 4u,
                                                 targets[k]) != 0) {
                            cg->had_error = 1; break;
                        }
                    }
                    free(targets);
                    if (cg->had_error) break;

                    /* Dispatch sequence:
                     *   SUB     off, cond, lo
                     *   CMP_LTU inrange, off, n_range
                     *   BEQ     inrange, zero, +M     ; skip past JMPR
                     *   MOVI    two, 2
                     *   SHL     idx, off, two
                     *   MOVI(/MOVHI) base, table_off
                     *   ADD     addr, base, idx
                     *   LDW     target, addr
                     *   JMPR    target
                     *   JMP     default_bb            ; landing for BEQ */
                    uint8_t lo_reg = cg_alloc_reg(cg); if (cg->had_error) break;
                    cg_emit_load_const32(cg, lo_reg, lo);
                    if (cg->had_error) break;
                    uint8_t off_reg = cg_alloc_reg(cg); if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_SUB, off_reg, cond_reg, lo_reg));
                    uint8_t n_reg = cg_alloc_reg(cg); if (cg->had_error) break;
                    cg_emit_load_const32(cg, n_reg, (int32_t)n_range);
                    if (cg->had_error) break;
                    uint8_t inrange = cg_alloc_reg(cg); if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_CMP_LTU, inrange, off_reg, n_reg));

                    /* BEQ placeholder; backpatched once M is known. */
                    uint32_t beq_pos = cg->count;
                    cg_emit(cg, 0);

                    uint8_t two_reg = cg_alloc_reg(cg); if (cg->had_error) break;
                    cg_emit(cg, enc_i16(CVM_OP_MOVI, two_reg, 2));
                    uint8_t idx_reg = cg_alloc_reg(cg); if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_SHL, idx_reg, off_reg, two_reg));
                    uint8_t base_reg = cg_alloc_reg(cg); if (cg->had_error) break;
                    cg_emit_load_const32(cg, base_reg, (int32_t)table_off);
                    if (cg->had_error) break;
                    uint8_t addr_reg = cg_alloc_reg(cg); if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_ADD, addr_reg, base_reg, idx_reg));
                    uint8_t target_reg = cg_alloc_reg(cg); if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_LDW, target_reg, addr_reg, 0));
                    cg_emit(cg, enc_r(CVM_OP_JMPR, target_reg, 0, 0));

                    /* Backpatch the BEQ to land just after JMPR (i.e.
                     * on the default JMP below). The skip distance is
                     * always small (≤ 9 instructions) and well within
                     * imm8, so no relaxation fixup is needed. */
                    int32_t skip = (int32_t)(cg->count - (beq_pos + 1));
                    if (skip < -128 || skip > 127) {
                        ERR(cg->fn_name,
                            "internal: switch-table BEQ skip %d outside imm8 range",
                            skip);
                        cg->had_error = 1;
                        break;
                    }
                    cg->code[beq_pos] = enc_br(CVM_OP_BEQ, inrange,
                                               cg->zero_reg, (int8_t)skip);

                    cg_queue_fixup(cg, cg->count, default_bb, 8, 24);
                    cg_emit(cg, enc_i24(CVM_OP_JMP, 0));
                } else {
                    /* Chain form. cond_reg / case constants are taken in
                     * the cond's narrow width (computed above) — using
                     * SExt would sign-extend `i8 -1` to 0xFFFFFFFF and
                     * miss the 0xFF cond_reg. */
                    for (unsigned k = 1; k < n_succ; ++k) {
                        LLVMValueRef      case_val = cvm_llvm_get_switch_case_value(i, k);
                        LLVMBasicBlockRef case_bb  = LLVMGetSuccessor(i, k);
                        if (!case_val || !LLVMIsAConstantInt(case_val)) {
                            ERR(cg->fn_name,
                                "switch case operand is not a constant integer");
                            cg->had_error = 1;
                            break;
                        }
                        unsigned long long zv = LLVMConstIntGetZExtValue(case_val);
                        if (cond_w < 64) zv &= ((1ULL << cond_w) - 1ULL);
                        uint8_t case_reg = cg_alloc_reg(cg);
                        if (cg->had_error) break;
                        cg_emit_load_const32(cg, case_reg, (int32_t)zv);
                        if (cg->had_error) break;
                        uint8_t tmp = cg_alloc_reg(cg);
                        if (cg->had_error) break;
                        cg_emit(cg, enc_r(CVM_OP_CMP_EQ, tmp, cond_reg, case_reg));
                        cg_queue_fixup(cg, cg->count, case_bb, 24, 8);
                        cg_emit(cg, enc_br(CVM_OP_BNE, tmp, cg->zero_reg, 0));
                    }
                    if (cg->had_error) break;

                    cg_queue_fixup(cg, cg->count, default_bb, 8, 24);
                    cg_emit(cg, enc_i24(CVM_OP_JMP, 0));
                }
                break;
            }

            case LLVMRet: {
                if (LLVMGetNumOperands(i) > 0) {
                    uint8_t r = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    if (cg->had_error) break;
                    if (r != 0)
                        cg_emit(cg, enc_r(CVM_OP_MOV, 0, r, 0));
                }
                if (cg_sp_add(cg, cg->frame_bytes)) break;
                cg_emit(cg, enc_r(CVM_OP_RET, 0, 0, 0));
                break;
            }

            case LLVMAlloca:
                /* No code here: the alloca pointer was materialised in the
                 * prologue. cg_collect_allocas validated that the alloca is
                 * static-size and lives in the entry block. */
                break;

            case LLVMLoad: {
                /* LDB / LDH / LDW chosen by result-type width. LDB/LDH
                 * zero-extend; sign extension, if needed by the IR, is the
                 * job of a following SExt (lowered as SHL/SAR pair). */
                LLVMValueRef ptr_v = LLVMGetOperand(i, 0);
                if (LLVMGetTypeKind(LLVMTypeOf(ptr_v)) != LLVMPointerTypeKind) {
                    ERR(cg->fn_name, "load: address operand must be a pointer");
                    cg->had_error = 1;
                    break;
                }
                LLVMTypeRef  lty = LLVMTypeOf(i);
                LLVMTypeKind lk  = LLVMGetTypeKind(lty);
                uint8_t opc = 0;
                if (lk == LLVMPointerTypeKind || lk == LLVMFloatTypeKind) {
                    /* f32 shares the 32-bit register file with i32 (bitcast is
                     * a no-op), so a float load is just a word load. */
                    opc = CVM_OP_LDW;
                } else if (lk == LLVMIntegerTypeKind) {
                    unsigned w = LLVMGetIntTypeWidth(lty);
                    if (w == 1 || w == 8) opc = CVM_OP_LDB;
                    else if (w == 16)     opc = CVM_OP_LDH;
                    else if (w == 32)     opc = CVM_OP_LDW;
                }
                if (!opc) {
                    ERR(cg->fn_name,
                        "load: unsupported result type "
                        "(only i1/i8/i16/i32/f32/ptr supported)");
                    cg->had_error = 1;
                    break;
                }
                uint8_t addr = cg_reg_for(cg, ptr_v);
                uint8_t dst  = cg->regs[cg_lookup(cg, i)];
                cg_emit(cg, enc_r(opc, dst, addr, 0));
                break;
            }

            case LLVMStore: {
                /* STB / STH / STW by value-type width. Sub-word stores write
                 * only the relevant low byte/halfword, so upper bits in the
                 * source register don't need to be masked here. */
                LLVMValueRef val_v = LLVMGetOperand(i, 0);
                LLVMTypeRef  vty   = LLVMTypeOf(val_v);
                LLVMTypeKind vk    = LLVMGetTypeKind(vty);

                /* clang lowers struct/array zero-init and small copies with
                 * 8-byte `store i64` chunks even on a 32-bit target. The VM
                 * has no 64-bit register, but a *constant* i64 store (the
                 * zero-init case) splits cleanly into two 32-bit word stores
                 * at addr and addr+4. A dynamic i64 store can't arise — the
                 * type subset rejects i64 SSA values. */
                if (vk == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(vty) == 64) {
                    if (!LLVMIsAConstantInt(val_v)) {
                        ERR(cg->fn_name, "store: dynamic i64 value unsupported "
                                         "on a 32-bit VM");
                        cg->had_error = 1;
                        break;
                    }
                    unsigned long long cv = LLVMConstIntGetZExtValue(val_v);
                    uint8_t addr = cg_reg_for(cg, LLVMGetOperand(i, 1));
                    uint8_t lo   = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit_load_const32(cg, lo, (int32_t)(cv & 0xFFFFFFFFu));
                    cg_emit(cg, enc_r(CVM_OP_STW, 0, addr, lo));
                    /* high word at addr + 4 */
                    cg_movi_scratch(cg, 4);
                    uint8_t a4 = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_ADD, a4, addr, (uint8_t)CG_REG_SCRATCH));
                    uint8_t hi = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit_load_const32(cg, hi, (int32_t)(cv >> 32));
                    cg_emit(cg, enc_r(CVM_OP_STW, 0, a4, hi));
                    break;
                }

                uint8_t opc = 0;
                if (vk == LLVMPointerTypeKind || vk == LLVMFloatTypeKind) {
                    /* f32 is a 32-bit word in the register file — store as STW. */
                    opc = CVM_OP_STW;
                } else if (vk == LLVMIntegerTypeKind) {
                    unsigned w = LLVMGetIntTypeWidth(vty);
                    if (w == 1 || w == 8) opc = CVM_OP_STB;
                    else if (w == 16)     opc = CVM_OP_STH;
                    else if (w == 32)     opc = CVM_OP_STW;
                }
                if (!opc) {
                    ERR(cg->fn_name,
                        "store: unsupported value type "
                        "(only i1/i8/i16/i32/f32/ptr supported)");
                    cg->had_error = 1;
                    break;
                }
                uint8_t val  = cg_reg_for(cg, val_v);
                uint8_t addr = cg_reg_for(cg, LLVMGetOperand(i, 1));
                cg_emit(cg, enc_r(opc, 0, addr, val));
                break;
            }

            /* Pointer/integer reinterprets are no-ops: pointers live in
             * 32-bit registers like every other scalar. Trunc/ZExt are MOVs
             * because LDB/LDH already zero-extend on load and STB/STH only
             * write the low byte/halfword on store, so upper-bit garbage in
             * a "narrow" register never reaches memory. */
            case LLVMPtrToInt:
            case LLVMIntToPtr:
            case LLVMBitCast:
            case LLVMZExt: {
                uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                if (src != dst)
                    cg_emit(cg, enc_r(CVM_OP_MOV, dst, src, 0));
                break;
            }

            /* Trunc to iN must zero out the high (32 - N) bits. The natural
             * register convention (matching LDB/LDH and ZExt) is that
             * narrow values are stored zero-extended; if Trunc just MOV'd,
             * a subsequent equality compare against an `iN k` case
             * constant could see stale upper bits and miscompare. The mask
             * is materialised via MOVI/MOVHI (no immediate AND). For
             * Trunc-to-i32 (legal but a no-op) we degenerate to a MOV. */
            case LLVMTrunc: {
                uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                LLVMTypeRef dty = LLVMTypeOf(i);
                unsigned dw = LLVMGetIntTypeWidth(dty);
                if (dw >= 32) {
                    if (src != dst)
                        cg_emit(cg, enc_r(CVM_OP_MOV, dst, src, 0));
                } else {
                    uint32_t mask = (dw == 0) ? 0u
                                              : (uint32_t)(((uint64_t)1 << dw) - 1u);
                    cg_emit_load_const32(cg, (uint8_t)CG_REG_SCRATCH, (int32_t)mask);
                    cg_emit(cg, enc_r(CVM_OP_AND, dst, src,
                                      (uint8_t)CG_REG_SCRATCH));
                }
                break;
            }

            /* SExt has to actually sign-extend when the source is narrower
             * than i32 (since narrow loads zero-extend). We don't have an
             * immediate-form shift, so the recipe is:
             *     MOVI scratch, (32 - src_w)
             *     SHL  dst, src, scratch
             *     SAR  dst, dst, scratch
             * For src_w == 32 (e.g. SExt across same width — rare, but legal
             * IR after some passes), it degenerates to a MOV. */
            case LLVMSExt: {
                LLVMValueRef src_v = LLVMGetOperand(i, 0);
                uint8_t src = cg_reg_for(cg, src_v);
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                LLVMTypeRef  sty = LLVMTypeOf(src_v);
                unsigned src_w =
                    (LLVMGetTypeKind(sty) == LLVMIntegerTypeKind)
                        ? LLVMGetIntTypeWidth(sty) : 32;
                if (src_w >= 32) {
                    if (src != dst)
                        cg_emit(cg, enc_r(CVM_OP_MOV, dst, src, 0));
                } else {
                    int16_t shift = (int16_t)(32u - src_w);
                    cg_emit(cg, enc_i16(CVM_OP_MOVI,
                                        (uint8_t)CG_REG_SCRATCH, shift));
                    cg_emit(cg, enc_r(CVM_OP_SHL, dst, src,
                                      (uint8_t)CG_REG_SCRATCH));
                    cg_emit(cg, enc_r(CVM_OP_SAR, dst, dst,
                                      (uint8_t)CG_REG_SCRATCH));
                }
                break;
            }

            case LLVMGetElementPtr: {
                /* Lower as: dst = base + sum(index * element-stride).
                 *   - First index strides over the source element type.
                 *   - Subsequent indices descend into arrays / structs. */
                LLVMValueRef base_v = LLVMGetOperand(i, 0);
                uint8_t base = cg_reg_for(cg, base_v);
                uint8_t dst  = cg->regs[cg_lookup(cg, i)];

                LLVMTypeRef cur_ty = LLVMGetGEPSourceElementType(i);
                if (!cur_ty) {
                    ERR(cg->fn_name, "GEP source element type missing");
                    cg->had_error = 1;
                    break;
                }

                /* Accumulator: constant offset folded at compile time;
                 * dynamic offset accumulated in a register if any. */
                long long  const_off = 0;
                int        have_dyn  = 0;
                uint8_t    dyn_reg   = 0;
                unsigned   nidx      = LLVMGetNumIndices(i);

                for (unsigned k = 0; k < nidx; ++k) {
                    LLVMValueRef idx_v = LLVMGetOperand(i, k + 1);

                    /* Determine stride and the type to descend into. */
                    uint32_t stride;
                    LLVMTypeRef next_ty;
                    if (k == 0) {
                        stride  = (uint32_t)LLVMABISizeOfType(cg->globals->td,
                                                              cur_ty);
                        next_ty = cur_ty;
                    } else if (LLVMGetTypeKind(cur_ty) == LLVMArrayTypeKind) {
                        LLVMTypeRef et = LLVMGetElementType(cur_ty);
                        stride  = (uint32_t)LLVMABISizeOfType(cg->globals->td, et);
                        next_ty = et;
                    } else if (LLVMGetTypeKind(cur_ty) == LLVMStructTypeKind) {
                        if (!LLVMIsAConstantInt(idx_v)) {
                            ERR(cg->fn_name,
                                "GEP into struct requires constant index");
                            cg->had_error = 1;
                            break;
                        }
                        unsigned fi = (unsigned)LLVMConstIntGetZExtValue(idx_v);
                        const_off += (long long)
                            LLVMOffsetOfElement(cg->globals->td, cur_ty, fi);
                        next_ty = LLVMStructGetTypeAtIndex(cur_ty, fi);
                        cur_ty  = next_ty;
                        continue;
                    } else {
                        ERR(cg->fn_name,
                            "GEP descended into unsupported aggregate kind");
                        cg->had_error = 1;
                        break;
                    }

                    if (LLVMIsAConstantInt(idx_v)) {
                        long long ci = LLVMConstIntGetSExtValue(idx_v);
                        const_off += ci * (long long)stride;
                    } else {
                        /* Dynamic: emit  step = idx * stride; dyn += step. */
                        uint8_t idx_r   = cg_reg_for(cg, idx_v);
                        uint8_t stride_r = cg_alloc_reg(cg);
                        if (cg->had_error) break;
                        cg_emit_load_const32(cg, stride_r, (int32_t)stride);
                        uint8_t step_r = cg_alloc_reg(cg);
                        if (cg->had_error) break;
                        cg_emit(cg, enc_r(CVM_OP_MUL, step_r, idx_r, stride_r));
                        if (!have_dyn) {
                            dyn_reg = step_r;
                            have_dyn = 1;
                        } else {
                            uint8_t sum_r = cg_alloc_reg(cg);
                            if (cg->had_error) break;
                            cg_emit(cg, enc_r(CVM_OP_ADD, sum_r, dyn_reg, step_r));
                            dyn_reg = sum_r;
                        }
                    }
                    cur_ty = next_ty;
                }
                if (cg->had_error) break;

                /* Combine: dst = base + const_off + dyn_reg. */
                uint8_t cur = base;
                if (const_off != 0) {
                    if (const_off < INT32_MIN || const_off > INT32_MAX) {
                        ERR(cg->fn_name,
                            "GEP constant offset %lld is wider than i32",
                            const_off);
                        cg->had_error = 1;
                        break;
                    }
                    uint8_t off_r = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit_load_const32(cg, off_r, (int32_t)const_off);
                    uint8_t sum_r = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_ADD, sum_r, cur, off_r));
                    cur = sum_r;
                }
                if (have_dyn) {
                    cg_emit(cg, enc_r(CVM_OP_ADD, dst, cur, dyn_reg));
                } else if (cur != dst) {
                    cg_emit(cg, enc_r(CVM_OP_MOV, dst, cur, 0));
                }
                break;
            }

            case LLVMCall: {
                LLVMValueRef callee = LLVMGetCalledValue(i);
                LLVMValueRef callee_fn = LLVMIsAFunction(callee);

                /* Indirect call (callee is an SSA value, not a Function
                 * constant). All the name-based dispatch below — intrinsics,
                 * syscalls, lifetime markers — only makes sense for direct
                 * calls, so for indirect we go straight to the user-call
                 * sequence at the bottom of this case. */
                const char *name = NULL;
                size_t name_len = 0;
                if (callee_fn) {
                    name = LLVMGetValueName2(callee_fn, &name_len);
                    if (!name || name_len == 0) {
                        ERR(cg->fn_name, "call with unnamed function");
                        cg->had_error = 1;
                        break;
                    }
                }
                if (!name) goto user_call_lowering;

                /* llvm.abs.i32(%x, _is_int_min_poison)
                 *   abs(x) = (x<0) ? -x : x
                 *
                 *   CMP_LT cond, x, zero
                 *   SUB    neg, zero, x
                 *   BEQ    cond, zero, +2   ; x>=0 ? skip true branch
                 *   MOV    dst, neg
                 *   JMP    +1
                 *   MOV    dst, x
                 */
                if (strcmp(name, "llvm.abs.i32") == 0) {
                    uint8_t x    = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t dst  = cg->regs[cg_lookup(cg, i)];
                    uint8_t cond = cg_alloc_reg(cg);
                    uint8_t neg  = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r (CVM_OP_CMP_LT, cond, x, cg->zero_reg));
                    cg_emit(cg, enc_r (CVM_OP_SUB, neg, cg->zero_reg, x));
                    cg_emit(cg, enc_br(CVM_OP_BEQ, cond, cg->zero_reg, 2));
                    cg_emit(cg, enc_r (CVM_OP_MOV, dst, neg, 0));
                    cg_emit(cg, enc_i24(CVM_OP_JMP, 1));
                    cg_emit(cg, enc_r (CVM_OP_MOV, dst, x, 0));
                    break;
                }

                /* llvm.fmuladd.f32(a, b, c) = a*b + c. clang emits it for
                 * float `a*b + c` under the default fp-contract — pervasive
                 * in matrix/vector maths. The VM has no fused op (and doesn't
                 * need the extra precision), so lower to FMUL + FADD. */
                if (strcmp(name, "llvm.fmuladd.f32") == 0) {
                    if (LLVMGetNumArgOperands(i) != 3) {
                        ERR(cg->fn_name, "llvm.fmuladd.f32 expects 3 args, got %u",
                            LLVMGetNumArgOperands(i));
                        cg->had_error = 1;
                        break;
                    }
                    uint8_t a   = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t b   = cg_reg_for(cg, LLVMGetOperand(i, 1));
                    uint8_t cc  = cg_reg_for(cg, LLVMGetOperand(i, 2));
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    uint8_t tmp = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_FMUL, tmp, a, b));
                    cg_emit(cg, enc_r(CVM_OP_FADD, dst, tmp, cc));
                    break;
                }

                /* Funnel-shift intrinsics — clang -O1 emits these for the
                 * canonical multi-precision shift pattern
                 *   `(a << n) | (b >> (32 - n))`.
                 * Spec (with c modulo 32):
                 *   fshl(a, b, c): top 32 bits of (a:b << c)  → a if c==0
                 *   fshr(a, b, c): bottom 32 bits of (a:b >> c) → b if c==0
                 *
                 * Lowered as 9 opcodes including a c==0 fixup. The VM's
                 * SHL/SHR mask the shift amount to its low 5 bits, so for
                 * c2=(c & 31)==0 the inverse-shift `b >> (32-c2)` becomes
                 * `b >> 0 == b`, contaminating the OR. The BNE+MOV at the
                 * end overrides dst with the correct identity value (a or
                 * b) when c2==0. Used by the soft-i64 / soft-double
                 * runtime headers for their multi-precision shifts. */
                if (strcmp(name, "llvm.fshl.i32") == 0 ||
                    strcmp(name, "llvm.fshr.i32") == 0)
                {
                    /* "llvm.fshl.i32" vs "llvm.fshr.i32" differ at index 8
                     * (the 'l' / 'r'); index 6 is 's' in both. */
                    int is_fshr = (name[8] == 'r');
                    if (LLVMGetNumArgOperands(i) != 3) {
                        ERR(cg->fn_name, "%s expects 3 args, got %u", name,
                            LLVMGetNumArgOperands(i));
                        cg->had_error = 1;
                        break;
                    }
                    uint8_t a    = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t b    = cg_reg_for(cg, LLVMGetOperand(i, 1));
                    uint8_t c    = cg_reg_for(cg, LLVMGetOperand(i, 2));
                    uint8_t dst  = cg->regs[cg_lookup(cg, i)];
                    uint8_t a_sh = cg_alloc_reg(cg);
                    uint8_t b_sh = cg_alloc_reg(cg);
                    uint8_t inv  = cg_alloc_reg(cg);
                    uint8_t c2   = cg_alloc_reg(cg);
                    if (cg->had_error) break;

                    /* Forward-shift first (a<<c for fshl, b>>c for fshr). */
                    if (is_fshr) cg_emit(cg, enc_r(CVM_OP_SHR, b_sh, b, c));
                    else         cg_emit(cg, enc_r(CVM_OP_SHL, a_sh, a, c));
                    /* inv = 32 - c. Materialise 32 in scratch, then SUB. */
                    cg_movi_scratch(cg, 32);
                    cg_emit(cg, enc_r(CVM_OP_SUB, inv,
                                      (uint8_t)CG_REG_SCRATCH, c));
                    /* Inverse shift on the other operand. */
                    if (is_fshr) cg_emit(cg, enc_r(CVM_OP_SHL, a_sh, a, inv));
                    else         cg_emit(cg, enc_r(CVM_OP_SHR, b_sh, b, inv));
                    cg_emit(cg, enc_r(CVM_OP_OR, dst, a_sh, b_sh));
                    /* Fixup for c2==0. */
                    cg_movi_scratch(cg, 31);
                    cg_emit(cg, enc_r(CVM_OP_AND, c2, c,
                                      (uint8_t)CG_REG_SCRATCH));
                    cg_emit(cg, enc_br(CVM_OP_BNE, c2, cg->zero_reg, 1));
                    cg_emit(cg, enc_r(CVM_OP_MOV, dst,
                                      is_fshr ? b : a, 0));
                    break;
                }

                /* `cvm_intrin_*` extern declarations from the runtime
                 * intrinsics header lower to a single opcode instead of a
                 * CALL — there's no body anywhere; the name exists only
                 * so clang has something to reference in the IR. */
                {
                    int is_mulh    = (strcmp(name, "cvm_intrin_mulh")       == 0);
                    int is_mulhu   = (strcmp(name, "cvm_intrin_mulhu")      == 0);
                    int is_f2i_s   = (strcmp(name, "cvm_intrin_f2i_sat_s")  == 0);
                    int is_f2i_u   = (strcmp(name, "cvm_intrin_f2i_sat_u")  == 0);
                    int is_fsqrt   = (strcmp(name, "cvm_intrin_fsqrt")      == 0);

                    int two_arg_op = -1;   /* MULH/MULHU shape */
                    int one_arg_op = -1;   /* F2I_* / FSQRT shape */
                    if (is_mulh)       two_arg_op = CVM_OP_MULH;
                    else if (is_mulhu) two_arg_op = CVM_OP_MULHU;
                    else if (is_f2i_s) one_arg_op = CVM_OP_F2I_S;
                    else if (is_f2i_u) one_arg_op = CVM_OP_F2I_U;
                    else if (is_fsqrt) one_arg_op = CVM_OP_FSQRT;

                    if (two_arg_op >= 0) {
                        if (LLVMGetNumArgOperands(i) != 2) {
                            ERR(cg->fn_name,
                                "%s expects 2 args, got %u", name,
                                LLVMGetNumArgOperands(i));
                            cg->had_error = 1;
                            break;
                        }
                        uint8_t lhs = cg_reg_for(cg, LLVMGetOperand(i, 0));
                        uint8_t rhs = cg_reg_for(cg, LLVMGetOperand(i, 1));
                        uint8_t dst = cg->regs[cg_lookup(cg, i)];
                        if (cg->had_error) break;
                        cg_emit(cg, enc_r((uint8_t)two_arg_op, dst, lhs, rhs));
                        break;
                    }
                    if (one_arg_op >= 0) {
                        if (LLVMGetNumArgOperands(i) != 1) {
                            ERR(cg->fn_name,
                                "%s expects 1 arg, got %u", name,
                                LLVMGetNumArgOperands(i));
                            cg->had_error = 1;
                            break;
                        }
                        uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
                        uint8_t dst = cg->regs[cg_lookup(cg, i)];
                        if (cg->had_error) break;
                        cg_emit(cg, enc_r((uint8_t)one_arg_op, dst, src, 0));
                        break;
                    }
                }

                /* min/max family. Lowered as cmp + branch + 2 MOVs. */
                int is_min = 0, is_signed = 0, is_minmax = 0;
                if (strcmp(name, "llvm.smax.i32") == 0) { is_minmax = 1; is_signed = 1; }
                else if (strcmp(name, "llvm.smin.i32") == 0) { is_minmax = 1; is_signed = 1; is_min = 1; }
                else if (strcmp(name, "llvm.umax.i32") == 0) { is_minmax = 1; }
                else if (strcmp(name, "llvm.umin.i32") == 0) { is_minmax = 1; is_min = 1; }

                if (is_minmax) {
                    uint8_t a    = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t b2   = cg_reg_for(cg, LLVMGetOperand(i, 1));
                    uint8_t dst  = cg->regs[cg_lookup(cg, i)];
                    uint8_t cond = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    /* cond = (a < b). For max, true -> b ; false -> a.
                     * For min, true -> a ; false -> b. */
                    uint8_t cmp_op = is_signed ? CVM_OP_CMP_LT : CVM_OP_CMP_LTU;
                    uint8_t true_v  = is_min ? a  : b2;
                    uint8_t false_v = is_min ? b2 : a;
                    cg_emit(cg, enc_r (cmp_op, cond, a, b2));
                    cg_emit(cg, enc_br(CVM_OP_BEQ, cond, cg->zero_reg, 2));
                    cg_emit(cg, enc_r (CVM_OP_MOV, dst, true_v, 0));
                    cg_emit(cg, enc_i24(CVM_OP_JMP, 1));
                    cg_emit(cg, enc_r (CVM_OP_MOV, dst, false_v, 0));
                    break;
                }

                /* Calls to cvm_sys_* lower to SYSCALL with the matching
                 * import index. Args go in R0..R(narg-1); return value
                 * comes back in R0. */
                if (strncmp(name, "cvm_sys_", 8) == 0) {
                    unsigned narg = LLVMGetNumArgOperands(i);
                    if (narg > 8) {
                        ERR(cg->fn_name,
                            "syscall '%s' has %u args; max 8", name, narg);
                        cg->had_error = 1;
                        break;
                    }
                    int sid = cg_import_lookup_or_add(cg, callee, name);
                    if (sid > 0xFFFF) {
                        ERR(cg->fn_name, "more than 65536 imports");
                        cg->had_error = 1;
                        break;
                    }
                    /* Read all arg sources before any MOV, so cg_reg_for
                     * for constants emits MOVI ahead of the arg moves. */
                    uint8_t arg_regs[8];
                    for (unsigned k = 0; k < narg; ++k)
                        arg_regs[k] = cg_reg_for(cg, LLVMGetOperand(i, k));
                    if (cg->had_error) break;
                    for (unsigned k = 0; k < narg; ++k)
                        if (arg_regs[k] != k)
                            cg_emit(cg, enc_r(CVM_OP_MOV,
                                              (uint8_t)k, arg_regs[k], 0));
                    cg_emit(cg, enc_i16(CVM_OP_SYSCALL, 0, (int16_t)sid));
                    LLVMTypeRef rty = LLVMTypeOf(i);
                    if (LLVMGetTypeKind(rty) != LLVMVoidTypeKind) {
                        uint8_t dst = cg->regs[cg_lookup(cg, i)];
                        if (dst != 0)
                            cg_emit(cg, enc_r(CVM_OP_MOV, dst, 0, 0));
                    }
                    break;
                }

                /* Block memory intrinsics — `llvm.memcpy.p0.p0.iN(dst, src,
                 * len, isvolatile)`, `llvm.memset.p0.iN(dst, val, len,
                 * isvolatile)`, `llvm.memmove.p0.p0.iN(dst, src, len,
                 * isvolatile)`. The fourth operand (i1 isvolatile) is
                 * compile-time metadata; the runtime ignores it. The length
                 * argument is i32 (i386-elf size_t); a wider variant would
                 * be rejected by the type-subset check upstream. */
                {
                    int is_memcpy  = (strncmp(name, "llvm.memcpy.",  12) == 0);
                    int is_memmove = (strncmp(name, "llvm.memmove.", 13) == 0);
                    int is_memset  = (strncmp(name, "llvm.memset.",  12) == 0);
                    if (is_memcpy || is_memmove || is_memset) {
                        unsigned narg = LLVMGetNumArgOperands(i);
                        if (narg < 3) {
                            ERR(cg->fn_name,
                                "intrinsic '%s' has %u args; expected >= 3",
                                name, narg);
                            cg->had_error = 1;
                            break;
                        }
                        /* Length is normally i32. clang also emits the i64
                         * variant (llvm.memset.p0.i64 etc.) — common for
                         * zeroing a large static or filling a struct, and it
                         * fires depending on flags like -ffreestanding. The
                         * VM is 32-bit, so a length over 4 GiB is impossible;
                         * accept i64 when it's a constant that fits in 32 bits
                         * (cg_reg_for then materialises it as a 32-bit
                         * immediate). A dynamic i64 length can't arise — the
                         * type subset rejects i64 SSA values upstream. */
                        LLVMValueRef len_v = LLVMGetOperand(i, 2);
                        LLVMTypeRef  lty   = LLVMTypeOf(len_v);
                        unsigned     lw    =
                            (LLVMGetTypeKind(lty) == LLVMIntegerTypeKind)
                                ? LLVMGetIntTypeWidth(lty) : 0;
                        if (lw != 32 && lw != 64) {
                            ERR(cg->fn_name,
                                "intrinsic '%s': length operand must be i32 or i64",
                                name);
                            cg->had_error = 1;
                            break;
                        }
                        if (lw == 64) {
                            if (!LLVMIsAConstantInt(len_v)) {
                                ERR(cg->fn_name,
                                    "intrinsic '%s': dynamic i64 length unsupported "
                                    "on a 32-bit VM", name);
                                cg->had_error = 1;
                                break;
                            }
                            if (LLVMConstIntGetZExtValue(len_v) > 0xFFFFFFFFull) {
                                ERR(cg->fn_name,
                                    "intrinsic '%s': length exceeds 32 bits", name);
                                cg->had_error = 1;
                                break;
                            }
                        }
                        uint8_t dst = cg_reg_for(cg, LLVMGetOperand(i, 0));
                        uint8_t mid = cg_reg_for(cg, LLVMGetOperand(i, 1));
                        uint8_t len = cg_reg_for(cg, LLVMGetOperand(i, 2));
                        if (cg->had_error) break;
                        uint8_t opc = is_memcpy  ? CVM_OP_MEMCPY
                                    : is_memmove ? CVM_OP_MEMMOVE
                                                 : CVM_OP_MEMSET;
                        cg_emit(cg, enc_r(opc, dst, mid, len));
                        break;
                    }
                }

                /* Lifetime markers are pure metadata in our world: they
                 * tell the optimiser when an alloca is in/out of use, but
                 * have no runtime effect. Drop them silently. */
                if (strncmp(name, "llvm.lifetime.", 14) == 0) {
                    break;
                }

                /* Variadic support. The i386 va_list is a single pointer to
                 * the next argument in memory. A variadic callee receives ALL
                 * its args on the stack (see the prologue and the LLVMCall
                 * handler), so the unnamed args sit contiguously just past the
                 * named ones. va_start writes that start address into the
                 * caller's va_list object; clang lowers va_arg itself to a
                 * load + pointer bump, so we never see an LLVMVAArg. va_end is
                 * a no-op; va_copy duplicates the pointer. */
                if (strncmp(name, "llvm.va_start", 13) == 0) {
                    uint8_t ap = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    if (cg->had_error) break;
                    unsigned n_named = LLVMCountParams(fn);
                    int32_t off = (int32_t)cg->frame_bytes + 4
                                + (int32_t)n_named * 4;
                    if (cg_addr_sp_plus(cg, off)) break;  /* SCRATCH = SP+off */
                    cg_emit(cg, enc_r(CVM_OP_STW, 0, ap,
                                      (uint8_t)CG_REG_SCRATCH));
                    break;
                }
                if (strncmp(name, "llvm.va_end", 11) == 0) {
                    break;   /* no runtime effect */
                }
                if (strncmp(name, "llvm.va_copy", 12) == 0) {
                    uint8_t dst = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 1));
                    if (cg->had_error) break;
                    uint8_t tmp = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_LDW, tmp, src, 0)); /* tmp=*src */
                    cg_emit(cg, enc_r(CVM_OP_STW, 0, dst, tmp)); /* *dst=tmp */
                    break;
                }

                if (strncmp(name, "llvm.", 5) == 0) {
                    ERR(cg->fn_name,
                        "intrinsic '%s' not yet lowered", name);
                    cg->had_error = 1;
                    break;
                }

            user_call_lowering: ;
                /* User-defined call. Direct (callee_fn != NULL): emit
                 * `CALL imm24` with the callee's index in the module's
                 * function table. Indirect (callee_fn == NULL): the
                 * callee is an SSA value of pointer type that holds a
                 * function index, emit `CALLR Rcallee`. */
                int callee_idx = -1;
                uint8_t callee_reg = 0;
                int is_indirect = (callee_fn == NULL);

                if (is_indirect) {
                    callee_reg = cg_reg_for(cg, callee);
                    if (cg->had_error) break;
                } else {
                    callee_idx = cg_func_lookup(cg, callee_fn);
                    if (callee_idx < 0) {
                        ERR(cg->fn_name,
                            "call to '%s': callee has no definition in this "
                            "module (extern is not supported)", name);
                        cg->had_error = 1;
                        break;
                    }
                    if (callee_idx >= 0xFFFFFF) {
                        /* +1 shift for the reserved FUNCS[0] slot must fit
                         * in CALL's imm24 field. */
                        ERR(cg->fn_name, "more than 16M user functions");
                        cg->had_error = 1;
                        break;
                    }
                }

                unsigned narg = LLVMGetNumArgOperands(i);
                /* A variadic callee takes ALL args on the stack (i386 vararg
                 * ABI) so its va_start finds them contiguous in memory; a
                 * normal callee uses the 8-in-regs convention. */
                LLVMTypeRef call_fty = LLVMGetCalledFunctionType(i);
                int call_is_vararg = call_fty && LLVMIsFunctionVarArg(call_fty);
                unsigned n_in_reg  = call_is_vararg ? 0u
                                   : (narg < 8 ? narg : 8);
                unsigned n_stacked = narg - n_in_reg;

                /* Materialise every argument into a register first, before
                 * any spill/copy. This makes sure constants get MOVI'd into
                 * their own scratch SSA registers (which we'll then spill
                 * harmlessly along with everything else). */
                uint8_t arg_regs[256];
                if (narg > sizeof arg_regs) {
                    ERR(cg->fn_name,
                        "call has %u args; codegen cap is %zu",
                        narg, sizeof arg_regs);
                    cg->had_error = 1;
                    break;
                }
                for (unsigned k = 0; k < narg; ++k)
                    arg_regs[k] = cg_reg_for(cg, LLVMGetOperand(i, k));
                if (cg->had_error) break;

                /* 1. Caller-saved spill, narrowed by liveness analysis.
                 *    `cg_compute_call_liveouts` precomputed, for each
                 *    LLVMCall, the set of SSA registers (R8..R(ssa_reg_high
                 *    -1)) that hold values used after the call returns.
                 *    Only those registers need to be preserved. The call's
                 *    own destination register is excluded explicitly: its
                 *    pre-call contents are garbage from this call's POV
                 *    (that register is exclusively assigned to the SSA
                 *    value the call is about to define), and the post-call
                 *    `MOV dst, R0` writes the actual return value, so a
                 *    spill/restore round-trip would just shuffle garbage.
                 *
                 *    Each spilled reg lands at slot_of[bit] within the
                 *    spill area (post-allocation compact index, not the
                 *    raw bit). slot_of[bit] == 0xFF means the analysis
                 *    didn't see this reg crossing any call, so no slot
                 *    exists and the spill must be skipped. */
                cg_bits spill_set;
                const cg_bits *live = cg_lookup_call_live(cg, i);
                if (live) {
                    spill_set = *live;
                    int my_bit = cg_spill_bit_of(cg, i);
                    if (my_bit >= 0)
                        cg_bits_clear_bit(&spill_set, (unsigned)my_bit);
                } else {
                    /* Defensive fallback: if liveness wasn't recorded
                     * for this call (shouldn't happen — every LLVMCall
                     * is snapshotted), spill the whole ever_spilled set.
                     * Anything outside it has no slot, so spilling it
                     * would be a bug anyway. */
                    spill_set = cg->ever_spilled;
                    int my_bit = cg_spill_bit_of(cg, i);
                    if (my_bit >= 0)
                        cg_bits_clear_bit(&spill_set, (unsigned)my_bit);
                }

                int spill_count = cg->ssa_reg_high - 8;
                for (int k = 0; k < spill_count; ++k) {
                    if (!cg_bits_test(&spill_set, (unsigned)k)) continue;
                    uint8_t slot = cg->slot_of[k];
                    if (slot == 0xFFu) continue;     /* no slot reserved */
                    int32_t off = (int32_t)cg->alloca_bytes + (int32_t)slot * 4;
                    if (cg_stw_sp_off(cg, off, (uint8_t)(8 + k))) break;
                }
                if (cg->had_error) break;

                /* 2. Push stacked args (the 9th onward) below the current
                 *    SP, lowest stacked arg at the lowest address. */
                if (n_stacked > 0) {
                    if (cg_sp_sub(cg, n_stacked * 4u)) break;
                    for (unsigned k = 0; k < n_stacked; ++k) {
                        if (cg_stw_sp_off(cg, (int32_t)(k * 4u),
                                          arg_regs[n_in_reg + k])) break;
                    }
                    if (cg->had_error) break;
                }

                /* 3. Move first-8 args into R0..R7. SSA homes are >= R8 so
                 *    they never alias targets — sequential MOVs are safe. */
                for (unsigned k = 0; k < n_in_reg; ++k) {
                    if (arg_regs[k] != k)
                        cg_emit(cg, enc_r(CVM_OP_MOV,
                                          (uint8_t)k, arg_regs[k], 0));
                }

                /* 4. CALL or CALLR. User functions occupy FUNCS[1..N]
                 *    (index 0 is reserved as the null-fn-ptr trap), so a
                 *    direct call uses (callee_idx + 1) as the imm24. */
                if (is_indirect) {
                    cg_emit(cg, enc_r(CVM_OP_CALLR, callee_reg, 0, 0));
                } else {
                    cg_emit(cg, enc_i24(CVM_OP_CALL, callee_idx + 1));
                }
                cg->has_calls = 1;

                /* 5. Pop stacked args (caller cleans). */
                if (n_stacked > 0) {
                    if (cg_sp_add(cg, n_stacked * 4u)) break;
                }

                /* 6. Restore the same registers we spilled in step 1
                 *    (and only those; an LDW with no matching STW would
                 *    load stale spill-area bytes from a previous call
                 *    site or uninitialised stack memory). slot_of[k]
                 *    must mirror step 1 exactly, so the same skip-on-
                 *    0xFF check applies. */
                for (int k = 0; k < spill_count; ++k) {
                    if (!cg_bits_test(&spill_set, (unsigned)k)) continue;
                    uint8_t slot = cg->slot_of[k];
                    if (slot == 0xFFu) continue;
                    int32_t off = (int32_t)cg->alloca_bytes + (int32_t)slot * 4;
                    if (cg_ldw_sp_off(cg, (uint8_t)(8 + k), off)) break;
                }
                if (cg->had_error) break;

                /* 7. Move R0 into the call's SSA home (after restore so
                 *    the restore doesn't clobber the just-set return). */
                LLVMTypeRef rty = LLVMTypeOf(i);
                if (LLVMGetTypeKind(rty) != LLVMVoidTypeKind) {
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    if (dst != 0)
                        cg_emit(cg, enc_r(CVM_OP_MOV, dst, 0, 0));
                }

                break;
            }

            default:
                ERR(cg->fn_name,
                    "%s: codegen not implemented yet", opcode_name(op));
                cg->had_error = 1;
                break;
            }
        }
    }

    /* Branch relaxation precedes fixup resolution: any imm8 fixup that
     * doesn't reach its target gets rewritten to a `BEQ +1; JMP imm24`
     * trampoline (3 instructions instead of 2). After this pass, every
     * remaining fixup is guaranteed to fit. */
    if (!cg->had_error && cg_relax_branches(cg) != 0)
        cg->had_error = 1;
    if (!cg->had_error && cg_resolve_fixups(cg) != 0)
        cg->had_error = 1;
    /* Switch jump-table entries are absolute instruction indices, so
     * they're patched after relaxation freezes block_offsets. */
    if (!cg->had_error && cg_resolve_table_fixups(cg) != 0)
        cg->had_error = 1;
    return cg->had_error ? 1 : 0;
}

/* --- binary writer ------------------------------------------------------- */

static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

struct cli_region {
    char     name[16];   /* NUL-terminated within 16 bytes */
    uint32_t size;
    uint32_t direction;  /* CVM_REGION_R / W / RW */
};

static int write_bin(const char *path,
                     const uint32_t *code, uint32_t code_count,
                     const uint8_t *data, uint32_t data_size,
                     uint32_t heap_reserve_size,
                     uint32_t stack_reserve_size,
                     const struct cg_import *imports, int import_count,
                     const struct cg_func *funcs, int func_count,
                     int emit_funcs,
                     const struct cli_region *regions, int region_count,
                     const uint8_t *rom, uint32_t rom_size,
                     uint32_t entry)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "translator: cannot open '%s' for writing: %s\n",
                path, strerror(errno));
        return 1;
    }

    /* Build IMPORTS payload up front so we know its size. */
    uint8_t  *imports_buf  = NULL;
    uint32_t  imports_size = 0;
    if (import_count > 0) {
        uint32_t names_off = 4u + (uint32_t)import_count * 4u;
        uint32_t total = names_off;
        for (int k = 0; k < import_count; ++k)
            total += (uint32_t)strlen(imports[k].name) + 1u;
        imports_buf  = (uint8_t *)calloc(1, total);
        imports_size = total;
        put_u32_le(imports_buf, (uint32_t)import_count);
        uint32_t cursor = names_off;
        for (int k = 0; k < import_count; ++k) {
            put_u32_le(imports_buf + 4u + (size_t)k * 4u, cursor);
            size_t L = strlen(imports[k].name);
            memcpy(imports_buf + cursor, imports[k].name, L + 1);
            cursor += (uint32_t)L + 1u;
        }
    }

    /* Build FUNCS payload (u32[N+1] of entry offsets). Slot 0 is reserved
     * as the null-function-pointer trap target; the interpreter rejects
     * any CALL/CALLR with fid==0 before reading FUNCS, so the value here
     * is unused — we leave it 0. User function k lives at FUNCS[k+1]. */
    uint8_t  *funcs_buf  = NULL;
    uint32_t  funcs_size = 0;
    if (emit_funcs && func_count > 0) {
        funcs_size = (uint32_t)(func_count + 1) * 4u;
        funcs_buf  = (uint8_t *)calloc(1, funcs_size);
        for (int k = 0; k < func_count; ++k)
            put_u32_le(funcs_buf + (size_t)(k + 1) * 4u, funcs[k].entry_offset);
    }

    /* Build HOST_REGION payload: u32 region_count followed by 28-byte
     * entries (name[16] + size + direction + flags). The loader assigns
     * offsets; the binary just declares what it needs. */
    uint8_t  *regions_buf  = NULL;
    uint32_t  regions_size = 0;
    if (region_count > 0) {
        regions_size = 4u + (uint32_t)region_count * 28u;
        regions_buf  = (uint8_t *)calloc(1, regions_size);
        put_u32_le(regions_buf, (uint32_t)region_count);
        for (int k = 0; k < region_count; ++k) {
            uint8_t *e = regions_buf + 4u + (size_t)k * 28u;
            memcpy(e, regions[k].name, 16);
            put_u32_le(e + 16, regions[k].size);
            put_u32_le(e + 20, regions[k].direction);
            put_u32_le(e + 24, 0u);
        }
    }

    uint32_t section_count = 1u
                           + (data_size          > 0 ? 1u : 0u)
                           + (imports_size       > 0 ? 1u : 0u)
                           + (heap_reserve_size  > 0 ? 1u : 0u)
                           + (stack_reserve_size > 0 ? 1u : 0u)
                           + (funcs_size         > 0 ? 1u : 0u)
                           + (regions_size       > 0 ? 1u : 0u)
                           + (rom_size           > 0 ? 1u : 0u);
    uint32_t table_off = 24;
    uint32_t code_off  = table_off + section_count * 16;
    uint32_t code_size = code_count * 4u;
    uint32_t data_off    = data_size    > 0 ? code_off + code_size : 0;
    uint32_t imports_off = imports_size > 0
                         ? code_off + code_size + data_size : 0;
    uint32_t funcs_off   = funcs_size > 0
                         ? code_off + code_size + data_size + imports_size : 0;
    uint32_t regions_off = regions_size > 0
                         ? code_off + code_size + data_size + imports_size
                                    + funcs_size : 0;
    uint32_t rom_off     = rom_size > 0
                         ? code_off + code_size + data_size + imports_size
                                    + funcs_size + regions_size : 0;

    uint8_t hdr[24] = {0};
    hdr[0] = 'C'; hdr[1] = 'V'; hdr[2] = 'M'; hdr[3] = '1';
    put_u32_le(hdr + 4,  CVM_VERSION_1_0);
    put_u32_le(hdr + 8,  0u);
    put_u32_le(hdr + 12, section_count);
    put_u32_le(hdr + 16, table_off);
    put_u32_le(hdr + 20, entry);
    fwrite(hdr, sizeof(hdr), 1, f);

    uint8_t sec[16] = {0};
    put_u32_le(sec + 0,  CVM_SEC_CODE);
    put_u32_le(sec + 4,  code_off);
    put_u32_le(sec + 8,  code_size);
    put_u32_le(sec + 12, 0u);
    fwrite(sec, sizeof(sec), 1, f);

    if (data_size > 0) {
        memset(sec, 0, sizeof(sec));
        put_u32_le(sec + 0,  CVM_SEC_DATA);
        put_u32_le(sec + 4,  data_off);
        put_u32_le(sec + 8,  data_size);
        put_u32_le(sec + 12, 0u);
        fwrite(sec, sizeof(sec), 1, f);
    }
    if (imports_size > 0) {
        memset(sec, 0, sizeof(sec));
        put_u32_le(sec + 0,  CVM_SEC_IMPORTS);
        put_u32_le(sec + 4,  imports_off);
        put_u32_le(sec + 8,  imports_size);
        put_u32_le(sec + 12, 0u);
        fwrite(sec, sizeof(sec), 1, f);
    }
    if (heap_reserve_size > 0) {
        memset(sec, 0, sizeof(sec));
        put_u32_le(sec + 0,  CVM_SEC_HEAP_RESERVE);
        put_u32_le(sec + 4,  0u);
        put_u32_le(sec + 8,  heap_reserve_size);
        put_u32_le(sec + 12, 0u);
        fwrite(sec, sizeof(sec), 1, f);
    }
    if (stack_reserve_size > 0) {
        memset(sec, 0, sizeof(sec));
        put_u32_le(sec + 0,  CVM_SEC_STACK_RESERVE);
        put_u32_le(sec + 4,  0u);
        put_u32_le(sec + 8,  stack_reserve_size);
        put_u32_le(sec + 12, 0u);
        fwrite(sec, sizeof(sec), 1, f);
    }
    if (funcs_size > 0) {
        memset(sec, 0, sizeof(sec));
        put_u32_le(sec + 0,  CVM_SEC_FUNCS);
        put_u32_le(sec + 4,  funcs_off);
        put_u32_le(sec + 8,  funcs_size);
        put_u32_le(sec + 12, 0u);
        fwrite(sec, sizeof(sec), 1, f);
    }
    if (regions_size > 0) {
        memset(sec, 0, sizeof(sec));
        put_u32_le(sec + 0,  CVM_SEC_HOST_REGION);
        put_u32_le(sec + 4,  regions_off);
        put_u32_le(sec + 8,  regions_size);
        put_u32_le(sec + 12, 0u);
        fwrite(sec, sizeof(sec), 1, f);
    }
    if (rom_size > 0) {
        memset(sec, 0, sizeof(sec));
        put_u32_le(sec + 0,  CVM_SEC_ROM);
        put_u32_le(sec + 4,  rom_off);
        put_u32_le(sec + 8,  rom_size);
        put_u32_le(sec + 12, 0u);
        fwrite(sec, sizeof(sec), 1, f);
    }

    for (uint32_t i = 0; i < code_count; ++i) {
        uint8_t b[4];
        put_u32_le(b, code[i]);
        fwrite(b, sizeof(b), 1, f);
    }

    if (data_size > 0)    fwrite(data, 1, data_size, f);
    if (imports_size > 0) fwrite(imports_buf, 1, imports_size, f);
    if (funcs_size > 0)   fwrite(funcs_buf, 1, funcs_size, f);
    if (regions_size > 0) fwrite(regions_buf, 1, regions_size, f);
    if (rom_size > 0)     fwrite(rom, 1, rom_size, f);
    free(imports_buf);
    free(funcs_buf);
    free(regions_buf);

    int err = ferror(f);
    fclose(f);
    if (err) {
        fprintf(stderr, "translator: write failed for '%s'\n", path);
        return 1;
    }
    return 0;
}

/* --- main ---------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
            "Usage: cvm-translate [-o <out.bin>] [--heap-reserve=N[K|M]] "
            "[--stack-reserve=N[K|M]] [--region=name:size[:rw|r|w]]... <input.bc>\n"
            "  -o <file>            Emit a CronoVM .bin (otherwise validate-only).\n"
            "  --heap-reserve=N     Reserve N bytes of free heap for the user\n"
            "                       allocator. K and M suffixes accepted.\n"
            "  --stack-reserve=N    Reserve N bytes of stack for CALL/RET.\n"
            "                       Default 16K when the module emits any CALL.\n"
            "  --region=NAME:SIZE[:DIR]\n"
            "                       Declare a host-shared region named NAME (max 15\n"
            "                       chars) of SIZE bytes. DIR is r, w, or rw\n"
            "                       (default rw). Repeatable.\n"
            "  --rom=FILE           Bake FILE's bytes into the .bin as read-only\n"
            "                       cartridge ROM (e.g. a game WAD). The program\n"
            "                       reads it via cvm_sys_rom_base/cvm_sys_rom_size.\n");
}

static int parse_size(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s) return 1;
    if (*end == 'K' || *end == 'k')      { v <<= 10; ++end; }
    else if (*end == 'M' || *end == 'm') { v <<= 20; ++end; }
    if (*end != '\0') return 1;
    if (v > 0x7FFFFFFFu) return 1;
    *out = (uint32_t)v;
    return 0;
}

#define CVM_DEFAULT_STACK_RESERVE (16u * 1024u)

/* Parse one --region=name:size[:rw|r|w]. Returns 0 on success. */
static int parse_region(const char *s, struct cli_region *out) {
    const char *colon1 = strchr(s, ':');
    if (!colon1 || colon1 == s) return 1;
    size_t nlen = (size_t)(colon1 - s);
    if (nlen >= 16) return 1;          /* leave room for the trailing NUL */
    memset(out->name, 0, sizeof(out->name));
    memcpy(out->name, s, nlen);
    /* Reject characters that would be ambiguous in our serialisation.
     * Names are arbitrary identifiers, so restrict to printable ASCII. */
    for (size_t i = 0; i < nlen; ++i) {
        unsigned char c = (unsigned char)out->name[i];
        if (c < 0x20 || c >= 0x7Fu) return 1;
    }

    const char *p = colon1 + 1;
    char *endp = NULL;
    unsigned long long v = strtoull(p, &endp, 0);
    if (endp == p) return 1;
    if (*endp == 'K' || *endp == 'k')      { v <<= 10; ++endp; }
    else if (*endp == 'M' || *endp == 'm') { v <<= 20; ++endp; }
    if (v == 0 || v > 0x7FFFFFFFu) return 1;
    out->size = (uint32_t)v;

    out->direction = CVM_REGION_RW;    /* default */
    if (*endp == ':') {
        ++endp;
        if      (strcmp(endp, "r")  == 0) out->direction = CVM_REGION_R;
        else if (strcmp(endp, "w")  == 0) out->direction = CVM_REGION_W;
        else if (strcmp(endp, "rw") == 0) out->direction = CVM_REGION_RW;
        else                              return 1;
    } else if (*endp != '\0') {
        return 1;
    }
    return 0;
}

#define MAX_CLI_REGIONS 64

/* --- fuzzing entry point -----------------------------------------------
 *
 * `cvm_fuzz_translate_buffer` exercises the parse + codegen pipeline
 * end-to-end on an in-memory bitcode blob, writing nothing to disk.
 * It is the unit-of-work for the libFuzzer harness in
 * `tools/translator/fuzz_translate.c`.
 *
 * Diagnostics that the CLI path prints to stderr (via the `ERR` macro
 * and `g_errors`) are intentionally left enabled — libFuzzer captures
 * stderr and only surfaces it on crash, so we get a useful tail when
 * something explodes. `g_errors` is reset on entry so iters are
 * independent.
 *
 * The return value is the rc the CLI would have produced; libFuzzer
 * ignores it (only crashes/ASAN/UBSAN reports matter). Memory is
 * fully released regardless of which step fails. */
int cvm_fuzz_translate_buffer(const uint8_t *data, size_t len);

/* LLVM's default LLVMContext diagnostic handler calls `exit(1)` for
 * DS_Error-level diagnostics, including the ones the bitcode parser
 * raises on malformed input ("file too small to contain bitcode
 * header", "Invalid bitcode signature", etc.). libFuzzer sees the
 * exit() as a crash and stops on the very first malformed input —
 * which, for random fuzz bytes, is basically every iteration.
 *
 * Installing a no-op handler swallows those diagnostics so the C-API
 * function returns a non-zero status the harness can handle in the
 * normal way. We don't care about the diagnostic text inside the
 * fuzz loop; the real signal is "did the translator pipeline crash,
 * leak, or trip ASAN/UBSAN?" */
static void cvm_fuzz_diag_handler(LLVMDiagnosticInfoRef di, void *ctx) {
    (void)di;
    (void)ctx;
}

int cvm_fuzz_translate_buffer(const uint8_t *data, size_t len) {
    g_errors = 0;

    LLVMMemoryBufferRef buf =
        LLVMCreateMemoryBufferWithMemoryRangeCopy(
            (const char *)data, len, "fuzz");
    if (!buf) return 1;

    LLVMContextRef ctx = LLVMContextCreate();
    LLVMContextSetDiagnosticHandler(ctx, cvm_fuzz_diag_handler, NULL);
    LLVMModuleRef  mod = NULL;
    if (LLVMParseBitcodeInContext2(ctx, buf, &mod)) {
        LLVMDisposeMemoryBuffer(buf);
        LLVMContextDispose(ctx);
        /* Not valid bitcode — by far the common case for random input.
         * Not a bug; tell libFuzzer to keep going. */
        return 1;
    }
    LLVMDisposeMemoryBuffer(buf);

    /* Validation pass mirrors main()'s. Suppressing the per-function
     * summary print keeps libFuzzer's stderr buffer manageable. */
    for (LLVMValueRef fn = LLVMGetFirstFunction(mod);
         fn; fn = LLVMGetNextFunction(fn))
    {
        if (LLVMIsDeclaration(fn)) continue;
        validate_function(fn);
    }

    int rc = 0;
    if (g_errors == 0) {
        struct cg_globals globals = {0};
        globals.td = LLVMCreateTargetData(LLVMGetDataLayoutStr(mod));

        struct cg cg = {0};
        cg.globals = &globals;

        for (LLVMValueRef fn = LLVMGetFirstFunction(mod);
             fn && rc == 0; fn = LLVMGetNextFunction(fn))
        {
            if (LLVMIsDeclaration(fn)) continue;
            if (cg_func_append(&cg, fn) < 0) { rc = 1; break; }
        }
        globals.funcs      = cg.funcs;
        globals.func_count = cg.func_count;

        if (rc == 0 && cg_collect_globals(&globals, mod) != 0) rc = 1;

        for (int k = 0; k < cg.func_count && rc == 0; ++k) {
            cg.funcs[k].entry_offset = cg.count;
            if (cg_function(&cg, cg.funcs[k].value, k) != 0) rc = 1;
        }

        free(cg.code);
        free(cg.vals);
        free(cg.regs);
        free(cg.blocks);
        free(cg.block_offsets);
        free(cg.fixups);
        free(cg.table_fixups);
        free(cg.imports);
        free(cg.funcs);
        free(cg.allocas);
        free(cg.bb_live_in);
        free(cg.bb_live_out);
        free(cg.call_lives);
        cg_globals_dispose(&globals);
    }

    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);

    if (g_errors > 0) return 1;
    return rc;
}

#ifndef CVM_NO_TRANSLATOR_MAIN
int main(int argc, char **argv) {
    const char *input  = NULL;
    const char *output = NULL;
    uint32_t    heap_reserve = 0;
    uint32_t    stack_reserve = 0;
    int         stack_reserve_set = 0;
    const char *rom_path = NULL;
    struct cli_region cli_regions[MAX_CLI_REGIONS];
    int         cli_region_count = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { usage(); return 2; }
            output = argv[++i];
        } else if (strncmp(argv[i], "--heap-reserve=", 15) == 0) {
            if (parse_size(argv[i] + 15, &heap_reserve) != 0) {
                fprintf(stderr, "translator: bad --heap-reserve value '%s'\n",
                        argv[i] + 15);
                return 2;
            }
        } else if (strncmp(argv[i], "--stack-reserve=", 16) == 0) {
            if (parse_size(argv[i] + 16, &stack_reserve) != 0) {
                fprintf(stderr, "translator: bad --stack-reserve value '%s'\n",
                        argv[i] + 16);
                return 2;
            }
            stack_reserve_set = 1;
        } else if (strncmp(argv[i], "--region=", 9) == 0) {
            if (cli_region_count >= MAX_CLI_REGIONS) {
                fprintf(stderr, "translator: too many --region (max %d)\n",
                        MAX_CLI_REGIONS);
                return 2;
            }
            struct cli_region *r = &cli_regions[cli_region_count];
            if (parse_region(argv[i] + 9, r) != 0) {
                fprintf(stderr, "translator: bad --region value '%s'\n",
                        argv[i] + 9);
                return 2;
            }
            for (int k = 0; k < cli_region_count; ++k) {
                if (strncmp(cli_regions[k].name, r->name, 16) == 0) {
                    fprintf(stderr,
                            "translator: duplicate --region name '%s'\n",
                            r->name);
                    return 2;
                }
            }
            cli_region_count++;
        } else if (strncmp(argv[i], "--rom=", 6) == 0) {
            rom_path = argv[i] + 6;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "translator: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (!input) {
            input = argv[i];
        } else {
            fprintf(stderr, "translator: extra positional argument '%s'\n",
                    argv[i]);
            return 2;
        }
    }
    if (!input) { usage(); return 2; }

    char *err_msg = NULL;
    LLVMMemoryBufferRef buf = NULL;
    if (LLVMCreateMemoryBufferWithContentsOfFile(input, &buf, &err_msg)) {
        fprintf(stderr, "translator: cannot read '%s': %s\n",
                input, err_msg ? err_msg : "(no detail)");
        LLVMDisposeMessage(err_msg);
        return 1;
    }

    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef mod = NULL;
    if (LLVMParseBitcodeInContext2(ctx, buf, &mod)) {
        fprintf(stderr, "translator: '%s' is not valid LLVM bitcode\n",
                input);
        LLVMDisposeMemoryBuffer(buf);
        LLVMContextDispose(ctx);
        return 1;
    }
    LLVMDisposeMemoryBuffer(buf);

    size_t src_len = 0;
    const char *src = LLVMGetSourceFileName(mod, &src_len);
    if (src && src_len) printf("module: %.*s\n", (int)src_len, src);

    LLVMValueRef main_fn = NULL;
    int defs = 0;
    for (LLVMValueRef fn = LLVMGetFirstFunction(mod);
         fn; fn = LLVMGetNextFunction(fn))
    {
        print_function_summary(fn);
        if (LLVMIsDeclaration(fn)) continue;
        ++defs;
        validate_function(fn);

        size_t nl = 0;
        const char *nm = LLVMGetValueName2(fn, &nl);
        if (!main_fn || (nm && nl == 4 && memcmp(nm, "main", 4) == 0))
            main_fn = fn;
    }
    if (defs == 0) ERR(NULL, "module contains no function definitions");

    int rc = 0;

    if (g_errors == 0 && output) {
        struct cg_globals globals = {0};
        globals.td = LLVMCreateTargetData(LLVMGetDataLayoutStr(mod));

        struct cg cg = {0};
        cg.globals = &globals;

        /* Pass 1: assign a stable function index to every definition.
         * Done before cg_collect_globals so the constant-initialiser
         * serialiser can map function values in DATA initialisers
         * (e.g. a `static const fn_t ops[3] = { add, sub, mul };`
         * dispatch table) to their FUNCS-table indices. Indices also
         * double as CALL imm24 operands, so they must exist before
         * any codegen for forward calls to work. */
        for (LLVMValueRef fn = LLVMGetFirstFunction(mod);
             fn && rc == 0; fn = LLVMGetNextFunction(fn))
        {
            if (LLVMIsDeclaration(fn)) continue;
            if (cg_func_append(&cg, fn) < 0) { rc = 1; break; }
        }
        globals.funcs      = cg.funcs;
        globals.func_count = cg.func_count;

        if (rc == 0 && cg_collect_globals(&globals, mod) != 0) {
            rc = 1;
        }

        int main_idx = -1;
        if (rc == 0 && main_fn) main_idx = cg_func_lookup(&cg, main_fn);
        if (rc == 0 && main_idx < 0) {
            ERR(NULL, "internal: no entry function selected");
            rc = 1;
        }

        /* Pass 2: emit code for each definition in declaration order.
         * Each function's entry_offset is recorded so the FUNCS section
         * and the header.entry field can reference it. */
        for (int k = 0; k < cg.func_count && rc == 0; ++k) {
            cg.funcs[k].entry_offset = cg.count;
            if (cg_function(&cg, cg.funcs[k].value, k) != 0) rc = 1;
        }

        /* Every translator-emitted function ends in RET, so even leaf
         * programs need at least the sentinel return PC on the stack.
         * Default to 16 KiB regardless of CALL usage; --stack-reserve
         * overrides. Setting it explicitly to 0 produces a binary
         * whose first RET will trap on out-of-bounds access. */
        uint32_t stack_size = stack_reserve_set
                            ? stack_reserve
                            : CVM_DEFAULT_STACK_RESERVE;

        /* Slurp the optional cartridge-ROM file. Lives in a separate buffer
         * appended to the .bin as a CVM_SEC_ROM section. */
        uint8_t *rom_buf = NULL;
        uint32_t rom_size = 0;
        if (rc == 0 && rom_path) {
            FILE *rf = fopen(rom_path, "rb");
            if (!rf) {
                fprintf(stderr, "translator: cannot read --rom file '%s': %s\n",
                        rom_path, strerror(errno));
                rc = 1;
            } else {
                fseek(rf, 0, SEEK_END);
                long n = ftell(rf);
                rewind(rf);
                if (n < 0 || (unsigned long)n > 0xFFFFFFFFu) {
                    fprintf(stderr, "translator: --rom file '%s' too large\n", rom_path);
                    rc = 1;
                } else if (n > 0) {
                    rom_buf = (uint8_t *)malloc((size_t)n);
                    if (!rom_buf || fread(rom_buf, 1, (size_t)n, rf) != (size_t)n) {
                        fprintf(stderr, "translator: failed reading --rom file '%s'\n", rom_path);
                        rc = 1;
                    } else {
                        rom_size = (uint32_t)n;
                    }
                }
                fclose(rf);
            }
        }

        if (rc == 0) {
            uint32_t entry_off = cg.funcs[main_idx].entry_offset;
            if (write_bin(output, cg.code, cg.count,
                          globals.data_bytes, globals.data_size,
                          heap_reserve, stack_size,
                          cg.imports, cg.import_count,
                          cg.funcs, cg.func_count,
                          cg.has_calls || cg.funcs_referenced,
                          cli_regions, cli_region_count,
                          rom_buf, rom_size,
                          entry_off) != 0)
            {
                rc = 1;
            } else {
                printf("translator: wrote %s (%u instructions, %u data bytes, "
                       "%d imports, %d funcs, %u heap-reserve, "
                       "%u stack-reserve, %d regions, %u rom-bytes)\n",
                       output, cg.count, globals.data_size,
                       cg.import_count, cg.func_count,
                       heap_reserve, stack_size, cli_region_count, rom_size);
            }
        }
        free(rom_buf);
        free(cg.code);
        free(cg.vals);
        free(cg.regs);
        free(cg.blocks);
        free(cg.block_offsets);
        free(cg.fixups);
        free(cg.table_fixups);
        free(cg.imports);
        free(cg.funcs);
        free(cg.allocas);
        free(cg.bb_live_in);
        free(cg.bb_live_out);
        free(cg.call_lives);
        cg_globals_dispose(&globals);
    }

    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);

    if (g_errors > 0) {
        fprintf(stderr,
                "translator: %d issue(s) — input is outside the supported subset\n",
                g_errors);
        return 1;
    }
    if (rc != 0) return rc;
    if (!output) printf("translator: ok\n");
    return 0;
}
#endif /* CVM_NO_TRANSLATOR_MAIN */
