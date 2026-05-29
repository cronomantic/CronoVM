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

/* --probe-runtime exit code: a bitmask of which soft runtimes the module
 * needs linked. 0 = none; the bits combine (e.g. 30 = both). Distinct from the
 * 1/2 used for IO/usage errors. cvm-cc relies on these exact values. */
#define CVM_PROBE_F64 10   /* uses double -> link cvm_float64_rt */
#define CVM_PROBE_I64 20   /* uses i64 div/rem -> link cvm_int64_rt */

static int g_errors = 0;

/* The instruction currently being lowered (set per-iteration by the codegen
 * loop, cleared otherwise). diag_v reads its debug location so errors carry
 * file:line — present only when the cart was built with line tables, i.e.
 * cvm-cc passed -gline-tables-only; harmless (no prefix) otherwise. */
static LLVMValueRef g_cur_inst = NULL;

static void diag_v(const char *func, const char *fmt, ...) {
    fprintf(stderr, "translator: ");
    if (g_cur_inst) {
        unsigned line = LLVMGetDebugLocLine(g_cur_inst);
        if (line) {
            unsigned flen = 0;
            const char *file = LLVMGetDebugLocFilename(g_cur_inst, &flen);
            if (file && flen) fprintf(stderr, "%.*s:%u: ", (int)flen, file, line);
        }
    }
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

/* Forward decls — 64-bit (i64/f64) type classification, defined with the
 * codegen helpers below but used by validate_function above them. */
static int cg_type_is_i64(LLVMTypeRef t);
static int cg_type_is_f64(LLVMTypeRef t);
static int cg_type_is_wide(LLVMTypeRef t);
static int cg_wide_const_words(LLVMValueRef v, uint32_t *lo, uint32_t *hi);

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
        /* i64 is legalised in codegen: a 64-bit value lives in two 32-bit
         * frame slots and every i64 operation is lowered to explicit lo/hi
         * word arithmetic (see cg_emit_i64_def). The ISA stays 32-bit. i64
         * across a function boundary (args/returns) is NOT yet handled —
         * validate_function rejects that separately. */
        if (bits == 64) return 1;
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
        /* `double` is legalised in codegen: an f64 value lives in two 32-bit
         * frame slots and each f64 op is lowered to an inline word op (fneg)
         * or a soft-float runtime call (cvm_emit_f64_def + the cvm_float64_rt
         * helpers). The ISA stays 32-bit. f64 across a function boundary
         * (args/returns) is NOT yet handled — validate_function rejects that. */
        return 1;
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
    /* freeze: lowered as identity (pass the operand through). clang emits it
     * to block poison propagation, e.g. around div/rem and the i64 legaliser's
     * volatile operands. Since the VM never exploits UB, identity is correct. */
    case LLVMFreeze:
    /* f32 ops — see translator's float codegen. FRem deliberately
     * excluded (no FREM opcode; users wanting fmod can call libc-style
     * helper from a future runtime header). FPExt/FPTrunc excluded
     * because they only apply to double, which we reject. */
    case LLVMFAdd: case LLVMFSub: case LLVMFMul: case LLVMFDiv:
    case LLVMFNeg: case LLVMFCmp:
    case LLVMSIToFP: case LLVMUIToFP: case LLVMFPToSI: case LLVMFPToUI:
    /* FPExt (float->double) and FPTrunc (double->float) are now accepted —
     * they only arise with f64, which the codegen legalises via soft-float. */
    case LLVMFPExt: case LLVMFPTrunc:
        return 1;
    default:
        return 0;
    }
}

static const char *reject_reason(LLVMOpcode op) {
    switch (op) {
    case LLVMFRem:
        return "frem not in the subset (no FREM opcode; call a fmod helper)";
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
    /* i64/f64 params and returns ARE supported (the 64-bit calling
     * convention: a wide value occupies two argument words — R-pair or two
     * stack words — and returns in R0:R1). */

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

static int cg_const_gep_offset(const struct cg_globals *g, LLVMValueRef gep,
                               long long *out);

/* Serialize a pointer-typed constant that addresses another global (a string
 * literal, a data global, or a pointer into one) as that global's data offset.
 * Globals live at heap offset 0 onward (DATA is the first heap section) and VM
 * pointers are heap offsets, so a global at data offset `o` simply has address
 * `o` — the same value codegen materialises for &global. No runtime relocation
 * is needed; we resolve the offset at translate time (which is why globals are
 * laid out in a first pass before any initializer is serialized). Returns 0 on
 * success, 1 if `c` is not a recognised global-address constant. */
static int serialize_global_ptr(const struct cg_globals *g, LLVMValueRef c,
                                uint8_t *out, uint32_t off, uint32_t sz)
{
    if (sz < 4) return 1;
    long long extra = 0;

    if (LLVMIsAConstantExpr(c)) {
        LLVMOpcode op = LLVMGetConstOpcode(c);
        if (op == LLVMBitCast || op == LLVMAddrSpaceCast ||
            op == LLVMIntToPtr || op == LLVMPtrToInt) {
            return serialize_global_ptr(g, LLVMGetOperand(c, 0), out, off, sz);
        }
        if (op == LLVMGetElementPtr) {
            if (cg_const_gep_offset(g, c, &extra) != 0) return 1;
            c = LLVMGetOperand(c, 0);   /* fall through with the base global */
        } else {
            return 1;
        }
    }

    if (!LLVMIsAGlobalVariable(c)) return 1;
    int idx = cg_globals_lookup(g, c);
    if (idx < 0) return 1;                 /* not laid out (should not happen) */
    uint32_t addr = g->items[idx].offset + (uint32_t)extra;
    out[off + 0] = (uint8_t)(addr);
    out[off + 1] = (uint8_t)(addr >> 8);
    out[off + 2] = (uint8_t)(addr >> 16);
    out[off + 3] = (uint8_t)(addr >> 24);
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
        LLVMBool lossy = 0;
        double   d     = LLVMConstRealGetDouble(c, &lossy);
        if (LLVMGetTypeKind(ty) == LLVMFloatTypeKind && sz == 4) {
            float    f = (float)d;
            uint32_t bits;
            memcpy(&bits, &f, sizeof bits);
            for (uint32_t b = 0; b < 4; ++b)
                out[off + b] = (uint8_t)(bits >> (b * 8));
            return 0;
        }
        /* f64 global (the soft-f64 runtime reads doubles as their native 8-byte
         * IEEE-754 little-endian image, so store exactly that). */
        if (LLVMGetTypeKind(ty) == LLVMDoubleTypeKind && sz == 8) {
            uint64_t bits;
            memcpy(&bits, &d, sizeof bits);
            for (uint32_t b = 0; b < 8; ++b)
                out[off + b] = (uint8_t)(bits >> (b * 8));
            return 0;
        }
        return 1;
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
    /* A pointer initializer holding the address of another global (string
     * literal / data global / a pointer into one). Resolved to that global's
     * data offset. */
    if (LLVMGetTypeKind(ty) == LLVMPointerTypeKind) {
        return serialize_global_ptr(g, c, out, off, sz);
    }
    return 1;
}

static int cg_collect_globals(struct cg_globals *g, LLVMModuleRef mod) {
    char err[256];

    /* Pass 1: lay out every global (assign a data offset) WITHOUT serializing.
     * An initializer may take the address of another global, so all offsets
     * must be known before any initializer is emitted (forward references). */
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
        if (cg_globals_append(g, gv, cursor, size) != 0) return 1;
        cursor += size;
        if (cursor > g->data_size) g->data_size = cursor;
    }

    /* Pass 2: serialize each initializer at its assigned offset. Now that the
     * layout is complete, address-of-global relocations resolve to offsets. */
    for (int i = 0; i < (int)g->count; ++i) {
        LLVMValueRef gv   = g->items[i].value;
        LLVMValueRef init = LLVMGetInitializer(gv);
        if (init && serialize_constant(g, init, g->data_bytes,
                                       g->items[i].offset, g->data_cap) != 0) {
            ERR(NULL, "global '%s': unsupported initializer shape "
                      "(or function-pointer initialiser without a "
                      "definition for the referenced function)",
                value_name(gv));
            return 1;
        }
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

/* --- Value spilling ------------------------------------------------------
 * When a function has more simultaneously-live SSA values than fit in the
 * register file, the excess values are SPILLED to per-value slots in the
 * stack frame (the "value-spill area"). The pre-allocator never assigns an
 * SSA value above CG_MAX_SSA_REG; the range R(CG_MAX_SSA_REG+1)..R252 is a
 * fixed EMIT SCRATCH WINDOW used during emission for:
 *   - materialised constants/globals/constant-GEP arithmetic (the existing
 *     transient mechanism, drawn from `next_reg` reset to ssa_reg_high each
 *     instruction), AND
 *   - reloads of spilled operands (cg_reg_for emits LDW into a transient
 *     drawn from the SAME pool), AND
 *   - the holding register for a spilled DEF (computed in a transient, then
 *     STW'd to its slot after the handler runs).
 * Because no SSA value ever occupies this window (ssa_reg_high <=
 * CG_MAX_SSA_REG+1), every instruction starts with the full window free, so
 * reloads and transients never collide with a live SSA value or with each
 * other — each gets a distinct register from next_reg++. The window (R230..
 * R252, 23 registers) comfortably exceeds the transient+reload demand of the
 * heaviest single instruction (the switch jump-table lowering allocates ~9
 * transients; fshl/fshr reads 3 operands + 4 transients). */
#define CG_MAX_SSA_REG 229u   /* SSA values use R8..R229. R230..R252 = emit
                                * scratch window. R253=zero, R254=scratch,
                                * R255=SP. */
#define CG_MAX_EMIT_REG 252u  /* highest register cg_alloc_reg may hand out
                                * for an emit-time transient/reload (the top
                                * of the emit scratch window). */

/* Sentinel placed in cg->regs[idx] for an SSA value that has been spilled
 * to a frame slot rather than kept in a register. Distinct from any real
 * SSA register (R8..R229) and from R0..R7 (which never hold a long-lived
 * SSA value — SSA homes start at R8). */
#define CG_REG_SPILLED 0u
/* Sentinel slot index meaning "value is NOT spilled" in cg->val_slot[]. */
#define CG_NO_SLOT     0xFFFFu

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
    uint16_t     *val_slot; /* value-spill slot index, or CG_NO_SLOT if the
                             * value lives in a register (regs[idx] != 0). A
                             * spilled value has regs[idx]==CG_REG_SPILLED and
                             * val_slot[idx] = its frame slot. Parallel to
                             * vals/regs. */
    uint16_t     *i64_slot; /* i64 legalisation: if != CG_NO_SLOT, this SSA
                             * value is a 64-bit integer living in TWO
                             * consecutive value-spill slots (lo at i64_slot,
                             * hi at i64_slot+1). Such a value never occupies a
                             * register: regs[idx]==CG_REG_SPILLED and
                             * val_slot[idx]==CG_NO_SLOT so the normal spill
                             * path ignores it. Parallel to vals/regs. */
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
    uint32_t          val_spill_bytes;/* value-spill area: val_spill_count * 4.
                                       * Holds SSA values that didn't fit in
                                       * registers (the linear allocator spilled
                                       * them on overflow). */
    int               val_spill_count;/* number of distinct value-spill slots */
    uint32_t          frame_bytes;    /* alloca_bytes + spill_bytes
                                       *               + val_spill_bytes */
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
    LLVMOpcode        cur_op;         /* opcode of instruction being emitted */

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
    cg->val_spill_bytes = 0;
    cg->val_spill_count = 0;
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
    /* Caps at CG_MAX_EMIT_REG (top of the emit scratch window). During
     * pre-alloc this only allocates parameters; an SSA-value def goes
     * through cg_pre_alloc_def, which caps at CG_MAX_SSA_REG and spills on
     * overflow. During emit it hands out transients/reloads from the
     * scratch window R(ssa_reg_high)..R252. Overflow here means a single
     * instruction demanded more scratch than the window holds — a real,
     * loud error, never silent corruption. */
    if (cg->next_reg > (int)CG_MAX_EMIT_REG) {
        ERR(cg->fn_name,
            "internal: emit scratch window exhausted (next_reg=%d > %u); a "
            "single instruction needs more transient/reload registers than "
            "R%u..R%u provides",
            cg->next_reg, CG_MAX_EMIT_REG,
            (unsigned)CG_MAX_SSA_REG + 1u, CG_MAX_EMIT_REG);
        if (getenv("CVM_SPILL_DEBUG"))
            fprintf(stderr, "[spill] exhaust in op %s\n", opcode_name(cg->cur_op));
        cg->had_error = 1;
        return 0;
    }
    return (uint8_t)cg->next_reg++;
}

static void cg_free_reg(struct cg *cg, uint8_t r) {
    /* Never push a register that's already pooled. A value used in several
     * operand slots of ONE instruction (e.g. `x*x` -> fmuladd(x, x, _), or any
     * `f(a, a)`) reaches the per-operand free loop once per occurrence; without
     * this guard its register would be pooled twice, and two later defs could
     * then both draw the SAME register and clobber each other (observed as an
     * infinite loop when the second def was a loop's exit `icmp` overwriting the
     * counter increment). Dedup keeps the pool a set. */
    for (int i = 0; i < cg->free_list_count; ++i)
        if (cg->free_list[i] == r) return;
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
        cg->val_slot = (uint16_t *)realloc(cg->val_slot,
                                      cg->map_cap * sizeof(*cg->val_slot));
        cg->i64_slot = (uint16_t *)realloc(cg->i64_slot,
                                      cg->map_cap * sizeof(*cg->i64_slot));
        if (!cg->vals || !cg->regs || !cg->val_slot || !cg->i64_slot) {
            perror("realloc"); exit(1);
        }
    }
    cg->vals[cg->map_count] = v;
    cg->regs[cg->map_count] = r;
    cg->val_slot[cg->map_count] = CG_NO_SLOT;
    cg->i64_slot[cg->map_count] = CG_NO_SLOT;
    cg->map_count++;
    return r;
}

/* Static byte offset of a constant-expression GEP. Every index of a
 * ConstantExpr GEP is a compile-time constant, so the whole offset folds.
 * Mirrors the index-walk in the LLVMGetElementPtr instruction handler.
 * Returns 0 on success (offset in *out), nonzero on an unsupported shape. */
static int cg_const_gep_offset(const struct cg_globals *g, LLVMValueRef gep, long long *out) {
    LLVMTypeRef cur_ty = LLVMGetGEPSourceElementType(gep);
    if (!cur_ty) return 1;
    long long off = 0;
    unsigned  nidx = LLVMGetNumIndices(gep);
    for (unsigned k = 0; k < nidx; ++k) {
        LLVMValueRef idx_v = LLVMGetOperand(gep, k + 1);
        if (!LLVMIsAConstantInt(idx_v)) return 1;
        if (k == 0) {
            long long ci = LLVMConstIntGetSExtValue(idx_v);
            off += ci * (long long)LLVMABISizeOfType(g->td, cur_ty);
        } else if (LLVMGetTypeKind(cur_ty) == LLVMArrayTypeKind) {
            LLVMTypeRef et = LLVMGetElementType(cur_ty);
            long long ci = LLVMConstIntGetSExtValue(idx_v);
            off += ci * (long long)LLVMABISizeOfType(g->td, et);
            cur_ty = et;
        } else if (LLVMGetTypeKind(cur_ty) == LLVMStructTypeKind) {
            unsigned fi = (unsigned)LLVMConstIntGetZExtValue(idx_v);
            off += (long long)LLVMOffsetOfElement(g->td, cur_ty, fi);
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
/* Forward decls for spill helpers used by cg_reg_for and phi lowering. */
static int32_t cg_val_slot_off(struct cg *cg, uint16_t slot);
static int cg_stw_sp_off(struct cg *cg, int32_t offset, uint8_t src_reg);
static int cg_ldw_sp_off(struct cg *cg, uint8_t dst_reg, int32_t offset);

/* Reload a spilled SSA operand into a fresh emit-scratch register and return
 * it. The register comes from cg_alloc_reg (the per-instruction transient
 * pool, R(ssa_reg_high)..R252), so every reloaded operand within one
 * instruction gets a DISTINCT register — they never clobber each other even
 * when a handler holds several operands live simultaneously (e.g. fshl reads
 * three operands across a multi-step sequence). cg_addr_sp_plus clobbers
 * SCRATCH (R254), which is never an SSA value, so the reload is
 * self-contained. */
static uint8_t cg_reload_spilled(struct cg *cg, int idx) {
    uint8_t reload = cg_alloc_reg(cg);
    if (cg->had_error) return 0;
    int32_t off = cg_val_slot_off(cg, cg->val_slot[idx]);
    if (cg_ldw_sp_off(cg, reload, off)) { cg->had_error = 1; return 0; }
    return reload;
}

static uint8_t cg_reg_for(struct cg *cg, LLVMValueRef v) {
    int idx = cg_lookup(cg, v);
    if (idx >= 0) {
        if (cg->val_slot[idx] != CG_NO_SLOT)
            return cg_reload_spilled(cg, idx);
        return cg->regs[idx];
    }

    if (LLVMIsAConstantInt(v)) {
        /* i1 booleans live as 0/1 in registers (icmp and the logic ops
         * produce 0/1). The *sign*-extended value of `i1 true` is -1, which
         * turns `xor i1 %x, true` (a one-bit flip, e.g. how clang lowers
         * `cond ? 0 : 1`) into a full-width NOT — silently miscompiling the
         * boolean. Materialise i1 constants zero-extended (0 or 1); wider
         * integer constants keep sign-extension (normalised at the use). */
        long long imm = (LLVMGetIntTypeWidth(LLVMTypeOf(v)) == 1)
                            ? (long long)LLVMConstIntGetZExtValue(v)
                            : LLVMConstIntGetSExtValue(v);
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
            if (cg_const_gep_offset(cg->globals, v, &off) != 0) {
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

/* Normalise an icmp operand to its comparison width `w` (only matters when
 * w < 32). The VM has no narrow compares, so both operands must agree in their
 * high bits. But the narrow-value register representation is NOT uniform:
 * loads zero-extend (LDB/LDH), Trunc masks, yet ZExt is a bare MOV and narrow
 * arithmetic leaves dirty high bits, while integer CONSTANTS are materialised
 * SIGN-extended (cg_reg_for). So a raw 32-bit compare of `iN` values is wrong:
 * e.g. `icmp eq i8 %v, -1` compares a zero-extended loaded 0x000000FF against a
 * sign-extended 0xFFFFFFFF and never matches — the infinite loop in
 * P_InitPicAnims's `animdefs[i].istexture != -1` terminator scan. Canonicalise
 * each operand per predicate signedness: zero-extend (AND mask) for
 * eq/ne/unsigned, sign-extend (SHL/SAR, as in the SExt lowering) for signed.
 * Uses CG_REG_SCRATCH for the mask/shift amount; cg_reg_for runs first (its
 * spill-reload path also clobbers SCRATCH) and the result lands in a fresh
 * transient so the two operands never alias. */
static uint8_t cg_icmp_operand(struct cg *cg, LLVMValueRef v,
                               unsigned w, int is_signed) {
    uint8_t src = cg_reg_for(cg, v);
    if (cg->had_error || w >= 32) return src;
    uint8_t dst = cg_alloc_reg(cg);
    if (cg->had_error) return src;
    if (is_signed) {
        int16_t shift = (int16_t)(32u - w);
        cg_emit(cg, enc_i16(CVM_OP_MOVI, (uint8_t)CG_REG_SCRATCH, shift));
        cg_emit(cg, enc_r(CVM_OP_SHL, dst, src, (uint8_t)CG_REG_SCRATCH));
        cg_emit(cg, enc_r(CVM_OP_SAR, dst, dst, (uint8_t)CG_REG_SCRATCH));
    } else {
        uint32_t mask = (uint32_t)(((uint64_t)1 << w) - 1u);
        cg_emit_load_const32(cg, (uint8_t)CG_REG_SCRATCH, (int32_t)mask);
        cg_emit(cg, enc_r(CVM_OP_AND, dst, src, (uint8_t)CG_REG_SCRATCH));
    }
    return dst;
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
/* A phi endpoint's location: a register, or a value-spill frame slot. We
 * encode a slot as (SLOT_FLAG | slot_index) so register and slot locations
 * inhabit one comparable namespace for parallel-copy conflict detection
 * (a slot can never collide with a register, but a slot CAN collide with
 * another slot when one phi feeds another). */
#define PHI_SLOT_FLAG 0x10000u
static int cg_loc_is_slot(unsigned loc) { return (loc & PHI_SLOT_FLAG) != 0; }

/* Resolve a phi destination (the phi result itself) to a location. */
static unsigned cg_phi_dst_loc(struct cg *cg, int phi_idx) {
    if (cg->val_slot[phi_idx] != CG_NO_SLOT)
        return PHI_SLOT_FLAG | cg->val_slot[phi_idx];
    return cg->regs[phi_idx];
}

/* Resolve a phi source value to a location WITHOUT clobbering registers when
 * it's a spilled SSA value (returns a slot loc, no LDW emitted). For
 * registers/constants/globals it falls through to cg_reg_for, which may emit
 * a materialising MOVI/MOVHI into a transient — that transient must stay live
 * until the write phase, which it does (next_reg only grows within this
 * call). */
static unsigned cg_phi_src_loc(struct cg *cg, LLVMValueRef src) {
    int idx = cg_lookup(cg, src);
    if (idx >= 0 && cg->val_slot[idx] != CG_NO_SLOT)
        return PHI_SLOT_FLAG | cg->val_slot[idx];
    return cg_reg_for(cg, src);   /* register (may materialise a constant) */
}

/* Read a phi source location into register `dst`. */
static void cg_phi_read_to(struct cg *cg, unsigned src_loc, uint8_t dst) {
    if (cg_loc_is_slot(src_loc)) {
        int32_t off = cg_val_slot_off(cg, (uint16_t)(src_loc & 0xFFFFu));
        if (cg_ldw_sp_off(cg, dst, off)) cg->had_error = 1;
    } else if ((uint8_t)src_loc != dst) {
        cg_emit(cg, enc_r(CVM_OP_MOV, dst, (uint8_t)src_loc, 0));
    }
}

/* Write register `src` into a phi destination location. */
static void cg_phi_write_from(struct cg *cg, unsigned dst_loc, uint8_t src) {
    if (cg_loc_is_slot(dst_loc)) {
        int32_t off = cg_val_slot_off(cg, (uint16_t)(dst_loc & 0xFFFFu));
        if (cg_stw_sp_off(cg, off, src)) cg->had_error = 1;
    } else if ((uint8_t)dst_loc != src) {
        cg_emit(cg, enc_r(CVM_OP_MOV, (uint8_t)dst_loc, src, 0));
    }
}

/* Resolve a WIDE (i64/f64) phi source's lo/hi word locations: slot locs for a
 * wide SSA value, materialised registers for a wide constant, the zero
 * register for undef. Mirrors cg_phi_src_loc for the two-word case. */
static void cg_phi_wide_src_locs(struct cg *cg, LLVMValueRef src,
                                 unsigned *lo, unsigned *hi) {
    int idx = cg_lookup(cg, src);
    if (idx >= 0 && cg->i64_slot[idx] != CG_NO_SLOT) {
        *lo = PHI_SLOT_FLAG | cg->i64_slot[idx];
        *hi = PHI_SLOT_FLAG | (unsigned)(cg->i64_slot[idx] + 1);
        return;
    }
    uint32_t clo, chi;
    if (cg_wide_const_words(src, &clo, &chi) == 0) {
        uint8_t rlo = cg_alloc_reg(cg); if (cg->had_error) return;
        cg_emit_load_const32(cg, rlo, (int32_t)clo);
        uint8_t rhi = cg_alloc_reg(cg); if (cg->had_error) return;
        cg_emit_load_const32(cg, rhi, (int32_t)chi);
        *lo = rlo; *hi = rhi;
        return;
    }
    if (LLVMIsUndef(src)) { *lo = cg->zero_reg; *hi = cg->zero_reg; return; }
    ERR(cg->fn_name, "internal: wide phi source without a slot or constant");
    cg->had_error = 1;
}

static void cg_emit_phi_moves(struct cg *cg,
                              LLVMBasicBlockRef from, LLVMBasicBlockRef to)
{
    /* A large function (e.g. a loop carrying hundreds of values) can have
     * many phis on one edge; size generously. The no-conflict path emits
     * each move through at most one transient, so an arbitrary count is
     * fine. The conflict (rotation) path stages through temps and is
     * bounded by the emit scratch window — a pathological rotation wider
     * than the window errors loudly rather than corrupting. */
    struct { unsigned src; unsigned dst; } moves[1024];
    int nmoves = 0;

    for (LLVMValueRef inst = LLVMGetFirstInstruction(to);
         inst && LLVMGetInstructionOpcode(inst) == LLVMPHI;
         inst = LLVMGetNextInstruction(inst))
    {
        unsigned n = LLVMCountIncoming(inst);
        for (unsigned k = 0; k < n; ++k) {
            if (LLVMGetIncomingBlock(inst, k) != from) continue;
            int phi_idx = cg_lookup(cg, inst);
            if (phi_idx < 0) {
                ERR(cg->fn_name, "internal: phi register not pre-allocated");
                cg->had_error = 1;
                return;
            }
            LLVMValueRef sval = LLVMGetIncomingValue(inst, k);
            if (nmoves + 2 > (int)(sizeof moves / sizeof moves[0])) {
                ERR(cg->fn_name,
                    "internal: too many phi moves on a single edge");
                cg->had_error = 1;
                return;
            }
            if (cg->i64_slot[phi_idx] != CG_NO_SLOT) {
                /* Wide (i64/f64) phi: two parallel word-moves into its slots.
                 * They flow through the same conflict detection / staging as
                 * scalar moves (slot locs are comparable). */
                unsigned dlo = PHI_SLOT_FLAG | cg->i64_slot[phi_idx];
                unsigned dhi = PHI_SLOT_FLAG |
                               (unsigned)(cg->i64_slot[phi_idx] + 1);
                unsigned slo, shi;
                cg_phi_wide_src_locs(cg, sval, &slo, &shi);
                if (cg->had_error) return;
                if (slo != dlo) { moves[nmoves].src = slo;
                                  moves[nmoves].dst = dlo; nmoves++; }
                if (shi != dhi) { moves[nmoves].src = shi;
                                  moves[nmoves].dst = dhi; nmoves++; }
                break;
            }
            unsigned dst_loc = cg_phi_dst_loc(cg, phi_idx);
            unsigned src_loc = cg_phi_src_loc(cg, sval);
            if (cg->had_error) return;
            if (src_loc == dst_loc) break;       /* no-op */
            moves[nmoves].src = src_loc;
            moves[nmoves].dst = dst_loc;
            nmoves++;
            break;
        }
    }

    /* Conflict iff any destination is also a source in the same batch
     * (register or slot — see PHI_SLOT_FLAG). */
    int conflict = 0;
    for (int i = 0; i < nmoves && !conflict; ++i)
        for (int j = 0; j < nmoves && !conflict; ++j)
            if (i != j && moves[i].dst == moves[j].src) conflict = 1;

    if (!conflict) {
        /* No write feeds a later read: emit each copy directly. A
         * register destination that is itself a constant-materialised
         * transient source is impossible (dsts are SSA homes/slots), so
         * direct emission preserves order. */
        for (int i = 0; i < nmoves; ++i) {
            if (!cg_loc_is_slot(moves[i].src)
                && !cg_loc_is_slot(moves[i].dst)) {
                cg_phi_write_from(cg, moves[i].dst, (uint8_t)moves[i].src);
            } else {
                /* Through one transient: LDW/MOV src -> tmp, then tmp ->
                 * dst (MOV/STW). Each move is independent (no conflict), and
                 * the transient is dead the instant the move completes, so we
                 * snapshot/restore next_reg to RECYCLE one scratch register
                 * across all moves — otherwise a loop carrying hundreds of
                 * spilled phis would walk next_reg off the end of the window. */
                int saved = cg->next_reg;
                uint8_t tmp = cg_alloc_reg(cg);
                if (cg->had_error) return;
                cg_phi_read_to(cg, moves[i].src, tmp);
                cg_phi_write_from(cg, moves[i].dst, tmp);
                cg->next_reg = saved;
            }
            if (cg->had_error) return;
        }
        return;
    }

    /* Conflict: read every source into its own temp, then write every
     * destination. Reads see pre-edge values, satisfying parallel phi
     * semantics even for rotations (a:=b; b:=a) and slot-to-slot feeds.
     * The temps come from the emit scratch window (cg_alloc_reg), which is
     * bounded; a rotation wider than the window errors loudly there rather
     * than corrupting. Real code rarely rotates more than a handful of
     * values on one edge. */
    uint8_t temps[1024];
    for (int i = 0; i < nmoves; ++i) {
        temps[i] = cg_alloc_reg(cg);
        if (cg->had_error) return;
        cg_phi_read_to(cg, moves[i].src, temps[i]);
        if (cg->had_error) return;
    }
    for (int i = 0; i < nmoves; ++i) {
        cg_phi_write_from(cg, moves[i].dst, temps[i]);
        if (cg->had_error) return;
    }
}

/* True if `to` carries phi moves on an edge from any predecessor — i.e. it
 * starts with a PHI. Phis sit at the block head, and every predecessor edge
 * (including `from`) appears in each phi, so one phi means this edge has moves.
 * Side-effect free (does NOT call cg_phi_src_loc, which would emit a constant
 * materialisation). Used by the conditional-branch handler to decide whether
 * phi-move copies must be ISOLATED to their edge — emitting them unconditionally
 * before the branch corrupts the other edge when a move's destination is a
 * value that edge still needs (the lost-copy problem: a loop back-edge writes
 * the induction phi register that the loop-exit edge reads). */
static int cg_block_has_phi(LLVMBasicBlockRef bb) {
    LLVMValueRef inst = LLVMGetFirstInstruction(bb);
    return inst && LLVMGetInstructionOpcode(inst) == LLVMPHI;
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

/* SP-relative byte offset of value-spill slot `slot`. The value-spill area
 * sits above the alloca and caller-save spill areas in the frame:
 *   [ alloca_bytes | spill_bytes (caller-save) | val_spill_bytes ]
 * so slot k is at alloca_bytes + spill_bytes + k*4. */
static int32_t cg_val_slot_off(struct cg *cg, uint16_t slot) {
    return (int32_t)cg->alloca_bytes + (int32_t)cg->spill_bytes
         + (int32_t)slot * 4;
}

/* --- i64 legalisation ----------------------------------------------------
 * The 32-bit register file can't hold a 64-bit value, so an i64 SSA value
 * lives in TWO consecutive value-spill slots (lo at i64_slot[idx], hi at
 * i64_slot[idx]+1, contiguous in the frame). An i64 value never occupies a
 * register; it bridges to the 32-bit world only by
 * being produced from / consumed into 32-bit values: sext/zext widen, trunc/
 * icmp/store narrow. Each i64 operation is lowered to explicit lo/hi word
 * arithmetic here. Scratch registers for the lo/hi temporaries come from
 * cg_alloc_reg (the per-instruction emit-scratch window R230..R252), which is
 * reset at the top of every instruction, so an i64 op's handful of temps
 * never collides with a live SSA value. */

static int cg_type_is_i64(LLVMTypeRef t) {
    return LLVMGetTypeKind(t) == LLVMIntegerTypeKind
        && LLVMGetIntTypeWidth(t) == 64;
}

static int cg_type_is_f64(LLVMTypeRef t) {
    return LLVMGetTypeKind(t) == LLVMDoubleTypeKind;
}

/* A "wide" value is any 64-bit scalar — i64 or f64. Both live in two
 * consecutive frame slots (the cg->i64_slot mechanism), differing only in how
 * their ops are lowered: i64 inline (cg_emit_i64_def), f64 via soft-float
 * runtime calls (cg_emit_f64_def). The slot read/write/storage code is shared. */
static int cg_type_is_wide(LLVMTypeRef t) {
    return cg_type_is_i64(t) || cg_type_is_f64(t);
}

/* True if instruction `i` lowers to a soft runtime CALL (and thus is a
 * caller-save spill point, like an LLVMCall). f64 arithmetic/comparison/
 * conversion ops call into cvm_float64_rt; i64 div/rem call into cvm_int64_rt.
 * Inline ops (fneg, i64 add/sub/mul/logic/shifts, f32 ops) do not count. */
static int cg_op_is_runtime_call(LLVMValueRef i, LLVMOpcode op) {
    switch (op) {
    case LLVMFAdd: case LLVMFSub: case LLVMFMul: case LLVMFDiv:
        return cg_type_is_f64(LLVMTypeOf(i));
    case LLVMSIToFP: case LLVMUIToFP: case LLVMFPExt:
        return cg_type_is_f64(LLVMTypeOf(i));
    case LLVMFCmp:
    case LLVMFPToSI: case LLVMFPToUI: case LLVMFPTrunc:
        return cg_type_is_f64(LLVMTypeOf(LLVMGetOperand(i, 0)));
    case LLVMUDiv: case LLVMSDiv: case LLVMURem: case LLVMSRem:
        return cg_type_is_i64(LLVMTypeOf(i));   /* i64 div/rem -> runtime call */
    case LLVMShl: case LLVMLShr: case LLVMAShr:
        /* i64 shift by a VARIABLE amount -> runtime call (constant is inline). */
        return cg_type_is_i64(LLVMTypeOf(i))
            && !LLVMIsAConstantInt(LLVMGetOperand(i, 1));
    case LLVMCall: {
        /* llvm.fmuladd/fma.f64 lower to two runtime calls; fabs/copysign are
         * inline. Only the former are spill points. */
        if (!cg_type_is_f64(LLVMTypeOf(i))) return 0;
        const char *cn = value_name(LLVMGetCalledValue(i));
        return strcmp(cn, "llvm.fmuladd.f64") == 0
            || strcmp(cn, "llvm.fma.f64") == 0
            || strcmp(cn, "llvm.sqrt.f64") == 0
            || strcmp(cn, "sqrt") == 0;
    }
    default:
        return 0;
    }
}

/* True if `i` is an f64-returning CALL that cg_emit_f64_def handles directly
 * (the soft-float intrinsics: fmuladd/fma/fabs/copysign/sqrt, plus a plain
 * `sqrt`). Such calls are diverted to cg_emit_f64_def; every OTHER wide-
 * returning call (a user or indirect function) goes through the generic
 * LLVMCall handler and the R0:R1 64-bit return ABI. */
static int cg_call_is_handled_f64_intrinsic(LLVMValueRef i) {
    if (LLVMGetInstructionOpcode(i) != LLVMCall) return 0;
    const char *cn = value_name(LLVMGetCalledValue(i));
    return strcmp(cn, "llvm.fmuladd.f64")  == 0
        || strcmp(cn, "llvm.fma.f64")      == 0
        || strcmp(cn, "llvm.fabs.f64")     == 0
        || strcmp(cn, "llvm.copysign.f64") == 0
        || strcmp(cn, "llvm.sqrt.f64")     == 0
        || strcmp(cn, "sqrt")              == 0;
}

/* SP-relative byte offset of the LO word of the i64 value at map index idx
 * (the HI word is at +4). */
static int32_t cg_i64_lo_off(struct cg *cg, int idx) {
    return cg_val_slot_off(cg, cg->i64_slot[idx]);
}

/* Decompose a 64-bit (i64 or f64) constant `v` into its lo/hi 32-bit words.
 * Returns 0 and writes both words on success, 1 if `v` isn't a 64-bit const. */
static int cg_wide_const_words(LLVMValueRef v, uint32_t *lo, uint32_t *hi) {
    if (LLVMIsAConstantInt(v)) {
        unsigned long long k = LLVMConstIntGetZExtValue(v);
        *lo = (uint32_t)(k & 0xFFFFFFFFu);
        *hi = (uint32_t)(k >> 32);
        return 0;
    }
    if (LLVMIsAConstantFP(v) && cg_type_is_f64(LLVMTypeOf(v))) {
        LLVMBool lossy = 0;
        double d = LLVMConstRealGetDouble(v, &lossy);
        uint64_t bits;
        memcpy(&bits, &d, sizeof bits);
        *lo = (uint32_t)(bits & 0xFFFFFFFFu);
        *hi = (uint32_t)(bits >> 32);
        return 0;
    }
    return 1;
}

/* Read a wide (i64 or f64) operand `v` into two fresh scratch registers
 * (*lo, *hi). Handles a 64-bit constant (materialised lo/hi words) or a wide
 * SSA value (loaded from its two frame slots). Returns 0 on success, 1 on
 * error (cg->had_error set). */
static int cg_i64_read(struct cg *cg, LLVMValueRef v, uint8_t *lo, uint8_t *hi) {
    uint32_t clo, chi;
    if (cg_wide_const_words(v, &clo, &chi) == 0) {
        *lo = cg_alloc_reg(cg); if (cg->had_error) return 1;
        cg_emit_load_const32(cg, *lo, (int32_t)clo);
        *hi = cg_alloc_reg(cg); if (cg->had_error) return 1;
        cg_emit_load_const32(cg, *hi, (int32_t)chi);
        return 0;
    }
    if (LLVMIsUndef(v)) {
        /* poison/undef 64-bit value — any concretisation is valid; use 0. */
        *lo = cg->zero_reg;
        *hi = cg->zero_reg;
        return 0;
    }
    int idx = cg_lookup(cg, v);
    if (idx < 0 || cg->i64_slot[idx] == CG_NO_SLOT) {
        ERR(cg->fn_name, "internal: i64 operand without a frame slot");
        cg->had_error = 1;
        return 1;
    }
    int32_t off = cg_i64_lo_off(cg, idx);
    *lo = cg_alloc_reg(cg); if (cg->had_error) return 1;
    if (cg_ldw_sp_off(cg, *lo, off))     { cg->had_error = 1; return 1; }
    *hi = cg_alloc_reg(cg); if (cg->had_error) return 1;
    if (cg_ldw_sp_off(cg, *hi, off + 4)) { cg->had_error = 1; return 1; }
    return 0;
}

/* Store (lo, hi) into the two frame slots of the i64 value being defined. */
static int cg_i64_write(struct cg *cg, int idx, uint8_t lo, uint8_t hi) {
    int32_t off = cg_i64_lo_off(cg, idx);
    if (cg_stw_sp_off(cg, off,     lo)) { cg->had_error = 1; return 1; }
    if (cg_stw_sp_off(cg, off + 4, hi)) { cg->had_error = 1; return 1; }
    return 0;
}

/* Emit `dst = src <shift> amt` where amt is a small constant. `cv` is one of
 * CVM_OP_SHL / CVM_OP_SHR / CVM_OP_SAR. The amount goes through R254 (the
 * shift opcode takes its count in a register; no immediate form). */
static void cg_shift_imm(struct cg *cg, uint8_t cv, uint8_t dst,
                         uint8_t src, unsigned amt) {
    cg_emit(cg, enc_i16(CVM_OP_MOVI, (uint8_t)CG_REG_SCRATCH, (int16_t)amt));
    cg_emit(cg, enc_r(cv, dst, src, (uint8_t)CG_REG_SCRATCH));
}

/* Forward decl: the generic soft-runtime call emitter (defined after the
 * liveness helpers it depends on). i64 div/rem use it like f64 ops do. */
static void cg_emit_runtime_call(struct cg *cg, LLVMValueRef i,
                                 const char *name, LLVMValueRef *wide_args,
                                 int n_wide, LLVMValueRef scalar_arg,
                                 int ret_slot, uint8_t dst_reg,
                                 unsigned scalar_sext_w);

/* `select` of a wide (i64/f64) value: result = cond ? a : b, both 64-bit.
 * Same branch shape as the i32 select but copying two words. `i` is the
 * select instruction; `idx` its map index (result slots = cg->i64_slot[idx]). */
static void cg_emit_wide_select(struct cg *cg, LLVMValueRef i, int idx) {
    uint8_t cond = cg_reg_for(cg, LLVMGetOperand(i, 0));
    if (cg->had_error) return;
    uint8_t al, ah, bl, bh;
    if (cg_i64_read(cg, LLVMGetOperand(i, 1), &al, &ah)) return;
    if (cg_i64_read(cg, LLVMGetOperand(i, 2), &bl, &bh)) return;
    uint8_t lo = cg_alloc_reg(cg); if (cg->had_error) return;
    uint8_t hi = cg_alloc_reg(cg); if (cg->had_error) return;
    /*   BEQ cond, zero, +3   ; cond==0 -> take the false (b) words
     *   MOV lo, al ; MOV hi, ah ; JMP +2
     *   MOV lo, bl ; MOV hi, bh */
    cg_emit(cg, enc_br(CVM_OP_BEQ, cond, cg->zero_reg, 3));
    cg_emit(cg, enc_r(CVM_OP_MOV, lo, al, 0));
    cg_emit(cg, enc_r(CVM_OP_MOV, hi, ah, 0));
    cg_emit(cg, enc_i24(CVM_OP_JMP, 2));
    cg_emit(cg, enc_r(CVM_OP_MOV, lo, bl, 0));
    cg_emit(cg, enc_r(CVM_OP_MOV, hi, bh, 0));
    cg_i64_write(cg, idx, lo, hi);
}

/* Lower an instruction whose RESULT is an i64 SSA value: sext/zext/load, the
 * inline arithmetic/logic/shift/mul ops, and div/rem (a soft runtime call).
 * Called from the codegen dispatch before the generic register-based switch.
 * The result is written to the value's two frame slots; nothing lands in a
 * register. */
static void cg_emit_i64_def(struct cg *cg, LLVMValueRef i, LLVMOpcode op) {
    int idx = cg_lookup(cg, i);
    if (idx < 0 || cg->i64_slot[idx] == CG_NO_SLOT) {
        ERR(cg->fn_name, "internal: i64 def without a frame slot");
        cg->had_error = 1;
        return;
    }

    switch (op) {
    case LLVMSExt: {
        /* lo = src; hi = src >>(arith) 31  (sign replicated). */
        uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
        if (cg->had_error) return;
        uint8_t hi = cg_alloc_reg(cg); if (cg->had_error) return;
        cg_shift_imm(cg, CVM_OP_SAR, hi, src, 31);
        cg_i64_write(cg, idx, src, hi);
        break;
    }
    case LLVMZExt: {
        /* lo = src; hi = 0. */
        uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
        if (cg->had_error) return;
        cg_i64_write(cg, idx, src, cg->zero_reg);
        break;
    }
    case LLVMLoad: {
        /* lo = [ptr]; hi = [ptr + 4]. */
        uint8_t addr = cg_reg_for(cg, LLVMGetOperand(i, 0));
        if (cg->had_error) return;
        uint8_t lo = cg_alloc_reg(cg); if (cg->had_error) return;
        cg_emit(cg, enc_r(CVM_OP_LDW, lo, addr, 0));
        cg_movi_scratch(cg, 4);
        uint8_t a4 = cg_alloc_reg(cg); if (cg->had_error) return;
        cg_emit(cg, enc_r(CVM_OP_ADD, a4, addr, (uint8_t)CG_REG_SCRATCH));
        uint8_t hi = cg_alloc_reg(cg); if (cg->had_error) return;
        cg_emit(cg, enc_r(CVM_OP_LDW, hi, a4, 0));
        cg_i64_write(cg, idx, lo, hi);
        break;
    }
    case LLVMAdd: case LLVMSub:
    case LLVMAnd: case LLVMOr: case LLVMXor: {
        uint8_t al, ah, bl, bh;
        if (cg_i64_read(cg, LLVMGetOperand(i, 0), &al, &ah)) return;
        if (cg_i64_read(cg, LLVMGetOperand(i, 1), &bl, &bh)) return;
        uint8_t lo = cg_alloc_reg(cg); if (cg->had_error) return;
        uint8_t hi = cg_alloc_reg(cg); if (cg->had_error) return;
        if (op == LLVMAdd) {
            /* lo = al + bl; carry = (lo <u al); hi = ah + bh + carry. */
            uint8_t carry = cg_alloc_reg(cg); if (cg->had_error) return;
            cg_emit(cg, enc_r(CVM_OP_ADD, lo, al, bl));
            cg_emit(cg, enc_r(CVM_OP_CMP_LTU, carry, lo, al));
            cg_emit(cg, enc_r(CVM_OP_ADD, hi, ah, bh));
            cg_emit(cg, enc_r(CVM_OP_ADD, hi, hi, carry));
        } else if (op == LLVMSub) {
            /* borrow = (al <u bl); lo = al - bl; hi = ah - bh - borrow. */
            uint8_t borrow = cg_alloc_reg(cg); if (cg->had_error) return;
            cg_emit(cg, enc_r(CVM_OP_CMP_LTU, borrow, al, bl));
            cg_emit(cg, enc_r(CVM_OP_SUB, lo, al, bl));
            cg_emit(cg, enc_r(CVM_OP_SUB, hi, ah, bh));
            cg_emit(cg, enc_r(CVM_OP_SUB, hi, hi, borrow));
        } else {
            uint8_t cv = (op == LLVMAnd) ? CVM_OP_AND
                       : (op == LLVMOr)  ? CVM_OP_OR : CVM_OP_XOR;
            cg_emit(cg, enc_r(cv, lo, al, bl));
            cg_emit(cg, enc_r(cv, hi, ah, bh));
        }
        cg_i64_write(cg, idx, lo, hi);
        break;
    }
    case LLVMMul: {
        /* 64-bit product, low 64 bits (signed and unsigned agree under
         * 2's complement). With a = ah:al, b = bh:bl:
         *   lo = al*bl
         *   hi = mulhu(al,bl) + al*bh + ah*bl   (the ah*bh*2^64 term drops)
         * Inline via MUL/MULHU — no runtime call. */
        uint8_t al, ah, bl, bh;
        if (cg_i64_read(cg, LLVMGetOperand(i, 0), &al, &ah)) return;
        if (cg_i64_read(cg, LLVMGetOperand(i, 1), &bl, &bh)) return;
        uint8_t lo = cg_alloc_reg(cg); if (cg->had_error) return;
        uint8_t hi = cg_alloc_reg(cg); if (cg->had_error) return;
        uint8_t t  = cg_alloc_reg(cg); if (cg->had_error) return;
        cg_emit(cg, enc_r(CVM_OP_MUL,   lo, al, bl));
        cg_emit(cg, enc_r(CVM_OP_MULHU, hi, al, bl));
        cg_emit(cg, enc_r(CVM_OP_MUL,   t,  al, bh));
        cg_emit(cg, enc_r(CVM_OP_ADD,   hi, hi, t));
        cg_emit(cg, enc_r(CVM_OP_MUL,   t,  ah, bl));
        cg_emit(cg, enc_r(CVM_OP_ADD,   hi, hi, t));
        cg_i64_write(cg, idx, lo, hi);
        break;
    }
    case LLVMFreeze: {
        /* identity: copy the operand's two words into the result slots. */
        uint8_t lo, hi;
        if (cg_i64_read(cg, LLVMGetOperand(i, 0), &lo, &hi)) break;
        cg_i64_write(cg, idx, lo, hi);
        break;
    }
    case LLVMSelect:
        cg_emit_wide_select(cg, i, idx);
        break;
    case LLVMUDiv: case LLVMSDiv: case LLVMURem: case LLVMSRem: {
        /* 64-bit divide/remainder is too large to open-code; lower to a
         * soft runtime call into cvm_int64_rt (sret result, like f64).
         * cvm-cc auto-links that TU when a module uses i64 div/rem. */
        const char *name = (op == LLVMUDiv) ? "__cvm_udiv64"
                         : (op == LLVMSDiv) ? "__cvm_sdiv64"
                         : (op == LLVMURem) ? "__cvm_umod64" : "__cvm_smod64";
        LLVMValueRef a2[2] = { LLVMGetOperand(i, 0), LLVMGetOperand(i, 1) };
        cg_emit_runtime_call(cg, i, name, a2, 2, NULL,
                             (int)cg->i64_slot[idx], 0, 0);
        break;
    }
    case LLVMLShr: case LLVMShl: case LLVMAShr: {
        LLVMValueRef amt_v = LLVMGetOperand(i, 1);
        if (!LLVMIsAConstantInt(amt_v)) {
            /* Variable amount: a soft runtime call. The amount is an i64 (LLVM
             * shift operands share the value type); the helper uses its low
             * word. Constant amounts stay inline below. */
            const char *name = (op == LLVMShl)  ? "__cvm_shl64"
                             : (op == LLVMLShr) ? "__cvm_shr64" : "__cvm_sar64";
            LLVMValueRef a2[2] = { LLVMGetOperand(i, 0), amt_v };
            cg_emit_runtime_call(cg, i, name, a2, 2, NULL,
                                 (int)cg->i64_slot[idx], 0, 0);
            return;
        }
        unsigned n = (unsigned)(LLVMConstIntGetZExtValue(amt_v) & 63u);
        uint8_t al, ah;
        if (cg_i64_read(cg, LLVMGetOperand(i, 0), &al, &ah)) return;
        uint8_t out_lo = 0, out_hi = 0;
        if (op == LLVMShl) {
            if (n == 0) { out_lo = al; out_hi = ah; }
            else if (n < 32) {
                /* hi = (ah << n) | (al >> (32-n)); lo = al << n. */
                uint8_t t1 = cg_alloc_reg(cg); if (cg->had_error) return;
                uint8_t t2 = cg_alloc_reg(cg); if (cg->had_error) return;
                out_hi = cg_alloc_reg(cg); if (cg->had_error) return;
                out_lo = cg_alloc_reg(cg); if (cg->had_error) return;
                cg_shift_imm(cg, CVM_OP_SHL, t1, ah, n);
                cg_shift_imm(cg, CVM_OP_SHR, t2, al, 32u - n);
                cg_emit(cg, enc_r(CVM_OP_OR, out_hi, t1, t2));
                cg_shift_imm(cg, CVM_OP_SHL, out_lo, al, n);
            } else if (n == 32) {
                out_hi = al; out_lo = cg->zero_reg;
            } else { /* 33..63 */
                out_hi = cg_alloc_reg(cg); if (cg->had_error) return;
                cg_shift_imm(cg, CVM_OP_SHL, out_hi, al, n - 32u);
                out_lo = cg->zero_reg;
            }
        } else {
            /* LShr (logical, fill 0) and AShr (arithmetic, fill sign). */
            uint8_t fill_op = (op == LLVMAShr) ? CVM_OP_SAR : CVM_OP_SHR;
            if (n == 0) { out_lo = al; out_hi = ah; }
            else if (n < 32) {
                /* lo = (al >> n) | (ah << (32-n)); hi = ah >>(fill) n. */
                uint8_t t1 = cg_alloc_reg(cg); if (cg->had_error) return;
                uint8_t t2 = cg_alloc_reg(cg); if (cg->had_error) return;
                out_lo = cg_alloc_reg(cg); if (cg->had_error) return;
                out_hi = cg_alloc_reg(cg); if (cg->had_error) return;
                cg_shift_imm(cg, CVM_OP_SHR, t1, al, n);
                cg_shift_imm(cg, CVM_OP_SHL, t2, ah, 32u - n);
                cg_emit(cg, enc_r(CVM_OP_OR, out_lo, t1, t2));
                cg_shift_imm(cg, fill_op, out_hi, ah, n);
            } else if (n == 32) {
                out_lo = ah;
                if (op == LLVMAShr) {
                    out_hi = cg_alloc_reg(cg); if (cg->had_error) return;
                    cg_shift_imm(cg, CVM_OP_SAR, out_hi, ah, 31);
                } else {
                    out_hi = cg->zero_reg;
                }
            } else { /* 33..63 */
                out_lo = cg_alloc_reg(cg); if (cg->had_error) return;
                cg_shift_imm(cg, fill_op, out_lo, ah, n - 32u);
                if (op == LLVMAShr) {
                    out_hi = cg_alloc_reg(cg); if (cg->had_error) return;
                    cg_shift_imm(cg, CVM_OP_SAR, out_hi, ah, 31);
                } else {
                    out_hi = cg->zero_reg;
                }
            }
        }
        cg_i64_write(cg, idx, out_lo, out_hi);
        break;
    }
    default:
        ERR(cg->fn_name, "i64 operation '%s' not yet supported "
                         "(mul/div/rem, phi, select and i64-returning calls "
                         "are not legalised yet)", opcode_name(op));
        cg->had_error = 1;
        break;
    }
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

/* Allocate a register for a value-producing instruction during pre-alloc.
 * Prefers the block-local free pool, then next_reg++. On overflow (no
 * register available in R8..R249), the value is SPILLED: it gets a fresh
 * value-spill slot, regs[idx] is set to CG_REG_SPILLED, and val_slot[idx]
 * records the slot. The instruction must already be cg_assign'd (idx valid).
 * Returns the assigned register, or CG_REG_SPILLED when spilled. */
static uint8_t cg_pre_alloc_def(struct cg *cg, int idx) {
    if (cg->free_list_count > 0) {
        uint8_t r = cg->free_list[--cg->free_list_count];
        cg->regs[idx] = r;
        return r;
    }
    if (cg->next_reg <= (int)CG_MAX_SSA_REG) {
        uint8_t r = (uint8_t)cg->next_reg++;
        cg->regs[idx] = r;
        return r;
    }
    /* Out of registers — spill this value to a frame slot. Each spilled
     * value gets its own slot (no interval reuse), which is always correct
     * and keeps the layout trivially non-overlapping. */
    cg->regs[idx] = (uint8_t)CG_REG_SPILLED;
    cg->val_slot[idx] = (uint16_t)cg->val_spill_count++;
    return (uint8_t)CG_REG_SPILLED;
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
        if (cg_type_is_wide(LLVMTypeOf(p))) {
            /* A 64-bit param lives in two frame slots (like any wide value),
             * fed from its two argument words by the prologue. It takes no
             * SSA register. */
            cg_assign(cg, p, (uint8_t)CG_REG_SPILLED);
            int idx = cg->map_count - 1;
            cg->i64_slot[idx] = (uint16_t)cg->val_spill_count;
            cg->val_spill_count += 2;
        } else {
            cg_assign(cg, p, cg_alloc_reg(cg));   /* R8..R(8+N-1) */
        }
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
            int dst_spilled = 0;
            LLVMTypeRef ty = LLVMTypeOf(i);
            if (cg_type_is_wide(ty)) {
                /* 64-bit result (i64 or f64): claim TWO consecutive value-spill
                 * slots (lo, hi) instead of a register. regs stays
                 * CG_REG_SPILLED and val_slot stays CG_NO_SLOT, so the
                 * linear-scan spill / caller-save protocol ignores it — the
                 * value lives purely in memory, accessed via cg->i64_slot in
                 * cg_emit_i64_def / cg_emit_f64_def. */
                cg_assign(cg, i, (uint8_t)CG_REG_SPILLED);
                int idx = cg->map_count - 1;
                cg->i64_slot[idx] = (uint16_t)cg->val_spill_count;
                cg->val_spill_count += 2;
            } else if (LLVMGetTypeKind(ty) != LLVMVoidTypeKind) {
                cg_assign(cg, i, (uint8_t)CG_REG_SPILLED); /* placeholder */
                int idx = cg->map_count - 1;
                dst_reg = cg_pre_alloc_def(cg, idx);
                if (cg->regs[idx] == (uint8_t)CG_REG_SPILLED
                    && cg->val_slot[idx] != CG_NO_SLOT)
                    dst_spilled = 1;
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
                if (cg->val_slot[v_idx] != CG_NO_SLOT) continue; /* spilled: no reg to free */
                if (cg->regs[v_idx] == (uint8_t)CG_REG_SPILLED) continue;
                if (!dst_spilled && (int)cg->regs[v_idx] == dst_reg) continue; /* dst owns it now */
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
    /* A value spilled to its own frame slot already lives in memory; it
     * holds no SSA register and so is never caller-saved across a call. */
    if (cg->val_slot[idx] != CG_NO_SLOT) return -1;
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
             * of registers live at the program point AFTER i has executed.
             * f64 ops that lower to a soft-float CALL are spill points too. */
            if (op == LLVMCall || cg_op_is_runtime_call(i, op)) {
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

/* --- f64 legalisation: soft-float runtime calls --------------------------
 * An f64 value lives in two frame slots (the shared cg->i64_slot mechanism).
 * Trivial ops (fneg/fabs, const, load/store) are lowered inline; arithmetic,
 * comparison and conversion ops are lowered to a CALL into the cvm_float64_rt
 * helpers (__cvm_f*), which cvm-cc links into any module that uses double.
 * The helpers use clang's i386 ABI: a cvm_f64 argument is two i32 words in
 * (lo,hi) order; a cvm_f64 return is via an sret hidden pointer (first arg).
 *
 * Reusing the existing caller-save spill machinery is what makes this safe:
 * each f64-runtime-call instruction was registered as a spill point in
 * cg_compute_call_liveouts, so the same liveness-narrowed save/restore that
 * protects a normal CALL protects these. */

/* Look up a module function by name; returns its FUNCS index or -1. */
static int cg_func_by_name(struct cg *cg, const char *name) {
    for (int k = 0; k < cg->func_count; ++k)
        if (strcmp(value_name(cg->funcs[k].value), name) == 0)
            return k;
    return -1;
}

/* The caller-save set for the call/runtime-call at `i`: its live-after
 * registers minus i's own destination (garbage pre-call, overwritten after). */
static cg_bits cg_call_spill_set(struct cg *cg, LLVMValueRef i) {
    const cg_bits *live = cg_lookup_call_live(cg, i);
    cg_bits set = live ? *live : cg->ever_spilled;
    int my_bit = cg_spill_bit_of(cg, i);
    if (my_bit >= 0) cg_bits_clear_bit(&set, (unsigned)my_bit);
    return set;
}

/* STW (cg_emit_spill) / LDW (cg_emit_restore) every register in `set` to/from
 * its compact caller-save slot at [SP + alloca_bytes + slot*4]. SP must be at
 * its normal frame position (no stacked-arg bias) — f64 calls take <=5 args,
 * all in registers, so SP never moves across the sequence. */
static int cg_emit_spill(struct cg *cg, const cg_bits *set) {
    int spill_count = cg->ssa_reg_high - 8;
    for (int k = 0; k < spill_count; ++k) {
        if (!cg_bits_test(set, (unsigned)k)) continue;
        uint8_t slot = cg->slot_of[k];
        if (slot == 0xFFu) continue;
        int32_t off = (int32_t)cg->alloca_bytes + (int32_t)slot * 4;
        if (cg_stw_sp_off(cg, off, (uint8_t)(8 + k))) return 1;
    }
    return 0;
}
static int cg_emit_restore(struct cg *cg, const cg_bits *set) {
    int spill_count = cg->ssa_reg_high - 8;
    for (int k = 0; k < spill_count; ++k) {
        if (!cg_bits_test(set, (unsigned)k)) continue;
        uint8_t slot = cg->slot_of[k];
        if (slot == 0xFFu) continue;
        int32_t off = (int32_t)cg->alloca_bytes + (int32_t)slot * 4;
        if (cg_ldw_sp_off(cg, (uint8_t)(8 + k), off)) return 1;
    }
    return 0;
}

/* Load f64/wide operand `v`'s lo word into R[base] and hi into R[base+1].
 * `v` is either a 64-bit SSA value (loaded from its frame slots) or a 64-bit
 * constant (materialised). Used for argument passing into the runtime call. */
static int cg_f64_operand_to_regs(struct cg *cg, LLVMValueRef v, uint8_t base) {
    uint32_t clo, chi;
    if (cg_wide_const_words(v, &clo, &chi) == 0) {
        cg_emit_load_const32(cg, base,     (int32_t)clo);
        cg_emit_load_const32(cg, base + 1, (int32_t)chi);
        return 0;
    }
    int idx = cg_lookup(cg, v);
    if (idx < 0 || cg->i64_slot[idx] == CG_NO_SLOT) {
        ERR(cg->fn_name, "internal: f64 operand without a frame slot");
        cg->had_error = 1;
        return 1;
    }
    int32_t off = cg_i64_lo_off(cg, idx);
    if (cg_ldw_sp_off(cg, base,     off))     { cg->had_error = 1; return 1; }
    if (cg_ldw_sp_off(cg, base + 1, off + 4)) { cg->had_error = 1; return 1; }
    return 0;
}

/* Emit one soft-float runtime call.
 *   name        : the __cvm_* callee (must be linked; resolved by name)
 *   i           : the LLVM instruction (caller-save liveness key)
 *   wide_args   : up to 2 f64 operands passed as (lo,hi) word pairs
 *   n_wide      : number of wide_args (0..2)
 *   scalar_arg  : an optional trailing i32/f32 operand (NULL if none)
 *   ret_slot    : >=0 -> sret result written to this frame slot (callee void);
 *                 <0  -> i32 result returned in R0
 *   dst_reg     : when ret_slot<0, the register that receives R0
 *   scalar_sext_w : if in [1,31], sign-extend the scalar arg from that bit
 *                 width to 32 bits (narrow loads/consts are zero-extended, so a
 *                 signed conversion like `sitofp i16` would otherwise see the
 *                 unsigned value). 0 / >=32 means leave the scalar as-is.
 * Argument register layout: [sret ptr if ret_slot>=0] then the wide pairs,
 * then the scalar — filling R0,R1,... in order. */
static void cg_emit_runtime_call(struct cg *cg, LLVMValueRef i, const char *name,
                             LLVMValueRef *wide_args, int n_wide,
                             LLVMValueRef scalar_arg,
                             int ret_slot, uint8_t dst_reg,
                             unsigned scalar_sext_w) {
    int callee_idx = cg_func_by_name(cg, name);
    if (callee_idx < 0) {
        ERR(cg->fn_name,
            "f64 runtime helper '%s' not found — link cvm_float64_rt.c "
            "(cvm-cc does this automatically when a module uses double)", name);
        cg->had_error = 1;
        return;
    }

    cg_bits set = cg_call_spill_set(cg, i);
    if (cg_emit_spill(cg, &set)) { cg->had_error = 1; return; }

    uint8_t r = 0;
    if (ret_slot >= 0) {
        /* R0 = &result_slot = SP + slot_off. */
        if (cg_addr_sp_plus(cg, cg_val_slot_off(cg, (uint16_t)ret_slot))) {
            cg->had_error = 1; return;
        }
        cg_emit(cg, enc_r(CVM_OP_MOV, 0, (uint8_t)CG_REG_SCRATCH, 0));
        r = 1;
    }
    for (int w = 0; w < n_wide; ++w) {
        if (cg_f64_operand_to_regs(cg, wide_args[w], r)) return;
        r += 2;
    }
    if (scalar_arg) {
        uint32_t clo, chi;
        if (LLVMIsAConstantInt(scalar_arg) || LLVMIsAConstantFP(scalar_arg)) {
            /* i32 / f32 constant: materialise its single word. (f32 const
             * bits are recovered by cg_reg_for, but here a direct const is
             * simplest for integer args; f32 args go through cg_reg_for.) */
            (void)chi;
            if (LLVMIsAConstantInt(scalar_arg)) {
                clo = (uint32_t)LLVMConstIntGetZExtValue(scalar_arg);
                cg_emit_load_const32(cg, r, (int32_t)clo);
            } else {
                uint8_t s = cg_reg_for(cg, scalar_arg);
                if (cg->had_error) return;
                if (s != r) cg_emit(cg, enc_r(CVM_OP_MOV, r, s, 0));
            }
        } else {
            uint8_t s = cg_reg_for(cg, scalar_arg);
            if (cg->had_error) return;
            if (s != r) cg_emit(cg, enc_r(CVM_OP_MOV, r, s, 0));
        }
        /* Sign-extend a narrow signed scalar (e.g. `sitofp i16`) in place: the
         * value arrived zero-extended, so without this __cvm_f_from_i32 would
         * read i16 -2008 as +63528. */
        if (scalar_sext_w >= 1 && scalar_sext_w < 32) {
            int16_t shift = (int16_t)(32u - scalar_sext_w);
            cg_emit(cg, enc_i16(CVM_OP_MOVI, (uint8_t)CG_REG_SCRATCH, shift));
            cg_emit(cg, enc_r(CVM_OP_SHL, r, r, (uint8_t)CG_REG_SCRATCH));
            cg_emit(cg, enc_r(CVM_OP_SAR, r, r, (uint8_t)CG_REG_SCRATCH));
        }
        r += 1;
    }

    cg_emit(cg, enc_i24(CVM_OP_CALL, callee_idx + 1));
    cg->has_calls = 1;

    if (cg_emit_restore(cg, &set)) { cg->had_error = 1; return; }

    if (ret_slot < 0 && dst_reg != 0)
        cg_emit(cg, enc_r(CVM_OP_MOV, dst_reg, 0, 0));
}

/* Number of argument WORDS a value of type `t` occupies in the calling
 * convention: 2 for a 64-bit (i64/f64) value, 1 for any scalar. */
static int cg_arg_words(LLVMTypeRef t) { return cg_type_is_wide(t) ? 2 : 1; }

/* Place ONE argument word into its calling-convention destination during a
 * user call. `av` is the argument value; `half` is -1 for a scalar arg (its
 * single word) or 0/1 for the lo/hi word of a 64-bit arg. `wp` is the word's
 * position in the flattened argument sequence: words < `n_reg_words` go into
 * R[wp]; the rest are stored at [SP + (wp - n_reg_words)*4] (SP already
 * dropped). `sp_bias` is added when reloading a spilled source. The emit
 * scratch window is recycled (next_reg snapshot) so an arbitrary arg count
 * never exhausts it. SSA sources are always >= R8 (or transient >= R230) and
 * constants materialise into transients, so writing R0..R7 never clobbers an
 * unconsumed source. */
static void cg_place_arg_word(struct cg *cg, LLVMValueRef av, int half,
                              unsigned wp, unsigned n_reg_words,
                              int32_t sp_bias) {
    int floor = cg->next_reg;
    int      target_reg = (wp < n_reg_words);
    uint8_t  rk         = (uint8_t)wp;                    /* if target_reg */
    int32_t  stk_off    = (int32_t)(wp - n_reg_words) * 4;/* if !target_reg */

    if (half < 0) {
        /* scalar arg (one word) */
        int aidx = cg_lookup(cg, av);
        if (aidx >= 0 && cg->val_slot[aidx] != CG_NO_SLOT) {
            int32_t voff = cg_val_slot_off(cg, cg->val_slot[aidx]) + sp_bias;
            if (target_reg) { cg_ldw_sp_off(cg, rk, voff); cg->next_reg = floor; return; }
            uint8_t s = cg_alloc_reg(cg); if (cg->had_error) { cg->next_reg = floor; return; }
            if (cg_ldw_sp_off(cg, s, voff)) { cg->next_reg = floor; return; }
            cg_stw_sp_off(cg, stk_off, s);
            cg->next_reg = floor;
            return;
        }
        uint8_t s = cg_reg_for(cg, av);
        if (cg->had_error) { cg->next_reg = floor; return; }
        if (target_reg) { if (s != rk) cg_emit(cg, enc_r(CVM_OP_MOV, rk, s, 0)); }
        else            cg_stw_sp_off(cg, stk_off, s);
        cg->next_reg = floor;
        return;
    }

    /* one word of a 64-bit arg */
    uint32_t clo, chi;
    if (cg_wide_const_words(av, &clo, &chi) == 0) {
        int32_t w = (int32_t)(half ? chi : clo);
        if (target_reg) { cg_emit_load_const32(cg, rk, w); cg->next_reg = floor; return; }
        uint8_t s = cg_alloc_reg(cg); if (cg->had_error) { cg->next_reg = floor; return; }
        cg_emit_load_const32(cg, s, w);
        cg_stw_sp_off(cg, stk_off, s);
        cg->next_reg = floor;
        return;
    }
    int aidx = cg_lookup(cg, av);
    if (aidx < 0 || cg->i64_slot[aidx] == CG_NO_SLOT) {
        ERR(cg->fn_name, "internal: 64-bit arg without a frame slot");
        cg->had_error = 1; cg->next_reg = floor; return;
    }
    int32_t off = cg_i64_lo_off(cg, aidx) + half * 4 + sp_bias;
    if (target_reg) { cg_ldw_sp_off(cg, rk, off); cg->next_reg = floor; return; }
    uint8_t s = cg_alloc_reg(cg); if (cg->had_error) { cg->next_reg = floor; return; }
    if (cg_ldw_sp_off(cg, s, off)) { cg->next_reg = floor; return; }
    cg_stw_sp_off(cg, stk_off, s);
    cg->next_reg = floor;
}

/* Lower an instruction whose RESULT is an f64 SSA value: arithmetic
 * (fadd/fsub/fmul/fdiv -> runtime call), fneg (inline sign flip), int/f32 ->
 * f64 conversions (sitofp/uitofp/fpext -> runtime call), and f64 load/const
 * (materialised into the result slots). Called from the dispatch before the
 * generic switch. */
static void cg_emit_f64_def(struct cg *cg, LLVMValueRef i, LLVMOpcode op) {
    int idx = cg_lookup(cg, i);
    if (idx < 0 || cg->i64_slot[idx] == CG_NO_SLOT) {
        ERR(cg->fn_name, "internal: f64 def without a frame slot");
        cg->had_error = 1;
        return;
    }
    int slot = cg->i64_slot[idx];

    switch (op) {
    case LLVMFAdd: case LLVMFSub: case LLVMFMul: case LLVMFDiv: {
        const char *name = (op == LLVMFAdd) ? "__cvm_fadd"
                         : (op == LLVMFSub) ? "__cvm_fsub"
                         : (op == LLVMFMul) ? "__cvm_fmul" : "__cvm_fdiv";
        LLVMValueRef a2[2] = { LLVMGetOperand(i, 0), LLVMGetOperand(i, 1) };
        cg_emit_runtime_call(cg, i, name, a2, 2, NULL, slot, 0, 0);
        break;
    }
    case LLVMFNeg: {
        /* Inline: copy operand words, flip the sign bit in the hi word. */
        uint8_t lo, hi;
        if (cg_i64_read(cg, LLVMGetOperand(i, 0), &lo, &hi)) break;
        cg_emit_load_const32(cg, (uint8_t)CG_REG_SCRATCH, (int32_t)0x80000000u);
        cg_emit(cg, enc_r(CVM_OP_XOR, hi, hi, (uint8_t)CG_REG_SCRATCH));
        cg_i64_write(cg, idx, lo, hi);
        break;
    }
    case LLVMSIToFP: case LLVMUIToFP: {
        const char *name = (op == LLVMSIToFP) ? "__cvm_f_from_i32"
                                              : "__cvm_f_from_u32";
        LLVMValueRef opnd = LLVMGetOperand(i, 0);
        unsigned sw = 0;
        if (op == LLVMSIToFP) {
            LLVMTypeRef t = LLVMTypeOf(opnd);
            if (LLVMGetTypeKind(t) == LLVMIntegerTypeKind) {
                unsigned w = LLVMGetIntTypeWidth(t);
                if (w < 32) sw = w;   /* narrow signed -> needs sign-extension */
            }
        }
        cg_emit_runtime_call(cg, i, name, NULL, 0, opnd, slot, 0, sw);
        break;
    }
    case LLVMFPExt: {
        /* float -> double. The f32 operand is one word (in a register). */
        cg_emit_runtime_call(cg, i, "__cvm_f_from_f32", NULL, 0,
                         LLVMGetOperand(i, 0), slot, 0, 0);
        break;
    }
    case LLVMLoad: {
        /* f64 load: lo = [ptr], hi = [ptr+4] — identical to the i64 path. */
        uint8_t addr = cg_reg_for(cg, LLVMGetOperand(i, 0));
        if (cg->had_error) return;
        uint8_t lo = cg_alloc_reg(cg); if (cg->had_error) return;
        cg_emit(cg, enc_r(CVM_OP_LDW, lo, addr, 0));
        cg_movi_scratch(cg, 4);
        uint8_t a4 = cg_alloc_reg(cg); if (cg->had_error) return;
        cg_emit(cg, enc_r(CVM_OP_ADD, a4, addr, (uint8_t)CG_REG_SCRATCH));
        uint8_t hi = cg_alloc_reg(cg); if (cg->had_error) return;
        cg_emit(cg, enc_r(CVM_OP_LDW, hi, a4, 0));
        cg_i64_write(cg, idx, lo, hi);
        break;
    }
    case LLVMFreeze: {
        /* identity: copy the operand's two words into the result slots. */
        uint8_t lo, hi;
        if (cg_i64_read(cg, LLVMGetOperand(i, 0), &lo, &hi)) break;
        cg_i64_write(cg, idx, lo, hi);
        break;
    }
    case LLVMSelect:
        cg_emit_wide_select(cg, i, idx);
        break;
    case LLVMCall: {
        /* f64-returning intrinsic calls. clang -O1 with the default
         * -ffp-contract=on synthesises llvm.fmuladd.f64 from `a*b±c`;
         * llvm.fabs.f64 / llvm.copysign.f64 also appear. */
        const char *cname = value_name(LLVMGetCalledValue(i));
        if (strcmp(cname, "llvm.fmuladd.f64") == 0 ||
            strcmp(cname, "llvm.fma.f64") == 0) {
            /* a*b + c, in two runtime calls. The intermediate product is
             * written to this value's OWN result slot, then read back as the
             * first addend (passing `i` as a wide operand references its slot,
             * which holds a*b after the first call). Two roundings, not one —
             * matching the soft runtime's existing truncating-mul accuracy and
             * the relaxed contract of fmuladd. */
            LLVMValueRef mul[2] = { LLVMGetOperand(i, 0), LLVMGetOperand(i, 1) };
            cg_emit_runtime_call(cg, i, "__cvm_fmul", mul, 2, NULL, slot, 0, 0);
            if (cg->had_error) break;
            LLVMValueRef add[2] = { i, LLVMGetOperand(i, 2) };
            cg_emit_runtime_call(cg, i, "__cvm_fadd", add, 2, NULL, slot, 0, 0);
            break;
        }
        /* `llvm.sqrt.f64` (clang with -fno-math-errno) or a plain `sqrt` call
         * (the default, errno-setting form — in a freestanding VM there is no
         * libm, so a double-returning `sqrt` is the math one). Both -> the
         * soft sqrt helper. */
        if (strcmp(cname, "llvm.sqrt.f64") == 0 || strcmp(cname, "sqrt") == 0) {
            LLVMValueRef a1[1] = { LLVMGetOperand(i, 0) };
            cg_emit_runtime_call(cg, i, "__cvm_fsqrt", a1, 1, NULL, slot, 0, 0);
            break;
        }
        if (strcmp(cname, "llvm.fabs.f64") == 0) {
            /* inline: copy operand words, clear the sign bit in the hi word. */
            uint8_t lo, hi;
            if (cg_i64_read(cg, LLVMGetOperand(i, 0), &lo, &hi)) break;
            cg_emit_load_const32(cg, (uint8_t)CG_REG_SCRATCH,
                                 (int32_t)0x7FFFFFFFu);
            cg_emit(cg, enc_r(CVM_OP_AND, hi, hi, (uint8_t)CG_REG_SCRATCH));
            cg_i64_write(cg, idx, lo, hi);
            break;
        }
        if (strcmp(cname, "llvm.copysign.f64") == 0) {
            /* hi = (mag.hi & 0x7FFFFFFF) | (sgn.hi & 0x80000000); lo = mag.lo */
            uint8_t mlo, mhi, slo, shi;
            if (cg_i64_read(cg, LLVMGetOperand(i, 0), &mlo, &mhi)) break;
            if (cg_i64_read(cg, LLVMGetOperand(i, 1), &slo, &shi)) break;
            (void)slo;
            cg_emit_load_const32(cg, (uint8_t)CG_REG_SCRATCH,
                                 (int32_t)0x7FFFFFFFu);
            cg_emit(cg, enc_r(CVM_OP_AND, mhi, mhi, (uint8_t)CG_REG_SCRATCH));
            cg_emit_load_const32(cg, (uint8_t)CG_REG_SCRATCH,
                                 (int32_t)0x80000000u);
            cg_emit(cg, enc_r(CVM_OP_AND, shi, shi, (uint8_t)CG_REG_SCRATCH));
            cg_emit(cg, enc_r(CVM_OP_OR, mhi, mhi, shi));
            cg_i64_write(cg, idx, mlo, mhi);
            break;
        }
        ERR(cg->fn_name,
            "f64-returning call '%s' not legalised (only llvm.fmuladd/fma/"
            "fabs/copysign.f64 are; a general double-returning function needs "
            "the 64-bit calling convention, a later phase)", cname);
        cg->had_error = 1;
        break;
    }
    default:
        ERR(cg->fn_name, "f64 operation '%s' not yet legalised",
            opcode_name(op));
        cg->had_error = 1;
        break;
    }
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
    /* Value-spill area (linear-scan overflow spills) sits on top of the
     * caller-save spill area. Each spilled value owns one i32 slot. */
    cg->val_spill_bytes = (uint32_t)cg->val_spill_count * 4u;
    /* Frame layout, low -> high address:
     *   [ alloca_bytes | spill_bytes (caller-save) | val_spill_bytes ]
     * Above SP (in the caller's frame): saved-PC, then stacked args/params
     * at SP + frame_bytes + 4 + i*4. */
    cg->frame_bytes = cg->alloca_bytes + cg->spill_bytes + cg->val_spill_bytes;
    cg->funcs[func_idx].frame_size = cg->frame_bytes;
    if (getenv("CVM_SPILL_DEBUG"))
        fprintf(stderr, "[spill] %s: ssa_high=%d val_spills=%d caller_save=%d\n",
                cg->fn_name, cg->ssa_reg_high, cg->val_spill_count,
                cg->spill_slot_count);

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
    /* Argument WORD position (not param index): a 64-bit param occupies two
     * consecutive words (lo, hi). Words 0..7 arrive in R0..R7 (non-vararg);
     * words >= 8 (and all words of a vararg callee) sit on the stack at
     * SP + frame + 4 + (word - 8 or word)*4. For scalar-only signatures word
     * == param index, so this is byte-identical to the old prologue. */
    unsigned word = 0;
    cg->next_reg = cg->ssa_reg_high;   /* emit-scratch window for wide loads */
    for (unsigned p = 0; p < n_params; ++p) {
        LLVMValueRef pv   = LLVMGetParam(fn, p);
        int          pidx = cg_lookup(cg, pv);
        int          wide = (cg->i64_slot[pidx] != CG_NO_SLOT);
        int          nw   = wide ? 2 : 1;
        if (!wide) {
            uint8_t dst = cg->regs[pidx];
            if (!fn_is_vararg && word < 8) {
                if (dst != word)
                    cg_emit(cg, enc_r(CVM_OP_MOV, dst, (uint8_t)word, 0));
            } else {
                unsigned slot = fn_is_vararg ? word : (word - 8);
                int32_t off = (int32_t)cg->frame_bytes + 4 + (int32_t)slot * 4;
                if (cg_ldw_sp_off(cg, dst, off)) return 1;
            }
        } else {
            /* Store each incoming word into the param's frame slot. */
            int32_t loff = cg_i64_lo_off(cg, pidx);
            for (int h = 0; h < 2; ++h) {
                unsigned wpos = word + (unsigned)h;
                if (!fn_is_vararg && wpos < 8) {
                    if (cg_stw_sp_off(cg, loff + h * 4, (uint8_t)wpos))
                        return 1;
                } else {
                    unsigned slot = fn_is_vararg ? wpos : (wpos - 8);
                    int32_t inoff =
                        (int32_t)cg->frame_bytes + 4 + (int32_t)slot * 4;
                    cg->next_reg = cg->ssa_reg_high;
                    uint8_t tmp = cg_alloc_reg(cg);
                    if (cg->had_error) return 1;
                    if (cg_ldw_sp_off(cg, tmp, inoff)) return 1;
                    if (cg_stw_sp_off(cg, loff + h * 4, tmp)) return 1;
                }
            }
        }
        word += (unsigned)nw;
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
            g_cur_inst = i;   /* for file:line in diagnostics */

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
            cg->cur_op = op;

            /* i64 legalisation: an instruction whose RESULT is i64 lives in
             * two frame slots, not a register, so lower it to lo/hi word ops
             * here and skip the generic register-based path entirely (its
             * result never needs the spilled-DEF machinery below). Insts that
             * merely CONSUME an i64 — trunc, icmp, store — keep their i32/void
             * result and are handled inside the switch. */
            /* A wide (i64/f64) RESULT lives in frame slots; lower it here and
             * skip the generic switch. PHI is the exception: like a scalar
             * phi it carries no code at its def site — its slots are written
             * by the incoming-edge moves (cg_emit_phi_moves), so let it fall
             * through to the (empty) PHI case below. */
            if (op != LLVMPHI && op != LLVMCall
                && cg_type_is_i64(LLVMTypeOf(i))) {
                cg_emit_i64_def(cg, i, op);
                if (cg->had_error) break;
                continue;
            }
            if (op != LLVMPHI && cg_type_is_f64(LLVMTypeOf(i))
                && (op != LLVMCall || cg_call_is_handled_f64_intrinsic(i))) {
                cg_emit_f64_def(cg, i, op);
                if (cg->had_error) break;
                continue;
            }
            /* A 64-bit-returning USER/indirect call falls through to the
             * generic LLVMCall handler (the R0:R1 return ABI). */

            /* Spilled-DEF setup: if this instruction's result was spilled to
             * a frame slot, give the handler a transient register to compute
             * into (drawn FIRST, before any operand reload, so it stays
             * reserved for the whole instruction). cg->regs[idx] is
             * temporarily redirected to that register so the existing
             * `dst = cg->regs[cg_lookup(cg, i)]` idiom in every handler writes
             * there transparently. After the switch we STW it to the slot and
             * restore the CG_REG_SPILLED marker. */
            int   result_idx     = cg_lookup(cg, i);
            /* PHIs are excluded: a spilled phi's slot is written by the
             * incoming-edge moves (cg_emit_phi_moves), not by this empty
             * PHI handler. Running the spilled-DEF store here would clobber
             * the freshly-resolved phi value with an uninitialised def_reg. */
            int   result_spilled = (result_idx >= 0
                                    && op != LLVMPHI
                                    && cg->val_slot[result_idx] != CG_NO_SLOT);
            uint8_t def_reg = 0;
            if (result_spilled) {
                def_reg = cg_alloc_reg(cg);
                if (cg->had_error) break;
                cg->regs[result_idx] = def_reg;   /* handler writes here */
            }

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
                /* i64 comparison: read both operands as lo/hi word pairs. */
                if (cg_type_is_i64(LLVMTypeOf(LLVMGetOperand(i, 0)))) {
                    uint8_t al, ah, bl, bh;
                    if (cg_i64_read(cg, LLVMGetOperand(i, 0), &al, &ah)) break;
                    if (cg_i64_read(cg, LLVMGetOperand(i, 1), &bl, &bh)) break;
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    if (p == LLVMIntEQ || p == LLVMIntNE) {
                        /* eq: (al==bl) & (ah==bh);  ne: (al!=bl) | (ah!=bh). */
                        uint8_t tlo = cg_alloc_reg(cg); if (cg->had_error) break;
                        uint8_t thi = cg_alloc_reg(cg); if (cg->had_error) break;
                        uint8_t cmp  = (p == LLVMIntEQ) ? CVM_OP_CMP_EQ : CVM_OP_CMP_NE;
                        uint8_t comb = (p == LLVMIntEQ) ? CVM_OP_AND    : CVM_OP_OR;
                        cg_emit(cg, enc_r(cmp, tlo, al, bl));
                        cg_emit(cg, enc_r(cmp, thi, ah, bh));
                        cg_emit(cg, enc_r(comb, dst, tlo, thi));
                        break;
                    }
                    /* Ordered compare. Normalise GT/GE to LT/LE by swapping
                     * operands, then:
                     *   a < b  ⟺  (ah <hi bh) | ((ah == bh) & (al <u bl))
                     *   a <= b ⟺  (ah <hi bh) | ((ah == bh) & (al <=u bl))
                     * The high word uses a SIGNED strict-less compare for the
                     * signed predicates and unsigned for the unsigned ones; the
                     * low word is ALWAYS unsigned. */
                    int is_le     = (p == LLVMIntSLE || p == LLVMIntULE ||
                                     p == LLVMIntSGE || p == LLVMIntUGE);
                    int is_signed = (p == LLVMIntSLT || p == LLVMIntSLE ||
                                     p == LLVMIntSGT || p == LLVMIntSGE);
                    int swap      = (p == LLVMIntSGT || p == LLVMIntSGE ||
                                     p == LLVMIntUGT || p == LLVMIntUGE);
                    if (swap) { uint8_t t;
                        t = al; al = bl; bl = t;
                        t = ah; ah = bh; bh = t; }
                    uint8_t hcmp = is_signed ? CVM_OP_CMP_LT : CVM_OP_CMP_LTU;
                    uint8_t lcmp = is_le     ? CVM_OP_CMP_LEU : CVM_OP_CMP_LTU;
                    uint8_t hi_lt = cg_alloc_reg(cg); if (cg->had_error) break;
                    uint8_t hi_eq = cg_alloc_reg(cg); if (cg->had_error) break;
                    uint8_t lo_r  = cg_alloc_reg(cg); if (cg->had_error) break;
                    cg_emit(cg, enc_r(hcmp, hi_lt, ah, bh));
                    cg_emit(cg, enc_r(CVM_OP_CMP_EQ, hi_eq, ah, bh));
                    cg_emit(cg, enc_r(lcmp, lo_r, al, bl));
                    cg_emit(cg, enc_r(CVM_OP_AND, lo_r, hi_eq, lo_r));
                    cg_emit(cg, enc_r(CVM_OP_OR, dst, hi_lt, lo_r));
                    break;
                }
                int swap = 0;
                int op2 = icmp_to_op(p, &swap);
                if (op2 < 0) {
                    ERR(cg->fn_name, "unsupported icmp predicate");
                    cg->had_error = 1;
                    break;
                }
                LLVMValueRef o0 = LLVMGetOperand(i, 0);
                LLVMValueRef o1 = LLVMGetOperand(i, 1);
                unsigned w = 32;
                LLVMTypeRef oty = LLVMTypeOf(o0);
                if (LLVMGetTypeKind(oty) == LLVMIntegerTypeKind)
                    w = LLVMGetIntTypeWidth(oty);
                int is_signed = (p == LLVMIntSLT || p == LLVMIntSLE ||
                                 p == LLVMIntSGT || p == LLVMIntSGE);
                uint8_t lhs = cg_icmp_operand(cg, o0, w, is_signed);
                uint8_t rhs = cg_icmp_operand(cg, o1, w, is_signed);
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
                /* f64 compare: lower to a single soft-float runtime call.
                 * Every predicate maps to one __cvm_f* helper plus an optional
                 * 0/1 negation (the unordered relationals are the negation of
                 * the complementary ordered compare; uno = !ord; ueq = !one). */
                if (cg_type_is_f64(LLVMTypeOf(LLVMGetOperand(i, 0)))) {
                    LLVMRealPredicate p = LLVMGetFCmpPredicate(i);
                    const char *fn = NULL;
                    int negate = 0;
                    switch (p) {
                    case LLVMRealOEQ: fn = "__cvm_feq";              break;
                    case LLVMRealUNE: fn = "__cvm_fne";              break;
                    case LLVMRealOLT: fn = "__cvm_flt";              break;
                    case LLVMRealOLE: fn = "__cvm_fle";              break;
                    case LLVMRealOGT: fn = "__cvm_fgt";              break;
                    case LLVMRealOGE: fn = "__cvm_fge";              break;
                    case LLVMRealUGE: fn = "__cvm_flt"; negate = 1;  break;
                    case LLVMRealUGT: fn = "__cvm_fle"; negate = 1;  break;
                    case LLVMRealULT: fn = "__cvm_fge"; negate = 1;  break;
                    case LLVMRealULE: fn = "__cvm_fgt"; negate = 1;  break;
                    case LLVMRealORD: fn = "__cvm_ford";             break;
                    case LLVMRealUNO: fn = "__cvm_ford"; negate = 1; break;
                    case LLVMRealONE: fn = "__cvm_fone";             break;
                    case LLVMRealUEQ: fn = "__cvm_fone"; negate = 1; break;
                    case LLVMRealPredicateTrue:
                    case LLVMRealPredicateFalse: {
                        uint8_t dst = cg->regs[cg_lookup(cg, i)];
                        cg_emit(cg, enc_i16(CVM_OP_MOVI, dst,
                                  (p == LLVMRealPredicateTrue) ? 1 : 0));
                        break;
                    }
                    default: break;
                    }
                    if (!fn) {
                        if (p == LLVMRealPredicateTrue ||
                            p == LLVMRealPredicateFalse) break;
                        ERR(cg->fn_name, "f64 fcmp predicate %d unsupported",
                            (int)p);
                        cg->had_error = 1;
                        break;
                    }
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    LLVMValueRef a2[2] = { LLVMGetOperand(i, 0),
                                           LLVMGetOperand(i, 1) };
                    cg_emit_runtime_call(cg, i, fn, a2, 2, NULL, -1, dst, 0);
                    if (cg->had_error) break;
                    if (negate) {
                        uint8_t one_r = cg_alloc_reg(cg);
                        if (cg->had_error) break;
                        cg_emit(cg, enc_i16(CVM_OP_MOVI, one_r, 1));
                        cg_emit(cg, enc_r(CVM_OP_SUB, dst, one_r, dst));
                    }
                    break;
                }
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
                 * compares unequal to itself.
                 * The unordered relational predicates (UGE/UGT/ULT/ULE) are the
                 * exact negation of the complementary ORDERED predicate — e.g.
                 * uge(a,b) ⇔ !(a<b ordered) — which is NaN-correct (a NaN makes
                 * the ordered compare false, so the negation is true, matching
                 * "unordered → true"). We compute the ordered compare and flip
                 * the 0/1 result with `1 - r`. ONE/UEQ are built from two LTs. */
                LLVMRealPredicate p = LLVMGetFCmpPredicate(i);
                int op2 = -1, swap = 0, negate = 0;
                int compound_kind = 0;       /* 0=normal,1=ord,2=uno,3=one,4=ueq */
                switch (p) {
                case LLVMRealOEQ: op2 = CVM_OP_FCMP_EQ;             break;
                case LLVMRealUNE: op2 = CVM_OP_FCMP_NE;             break;
                case LLVMRealOLT: op2 = CVM_OP_FCMP_LT;             break;
                case LLVMRealOLE: op2 = CVM_OP_FCMP_LE;             break;
                case LLVMRealOGT: op2 = CVM_OP_FCMP_LT; swap = 1;   break;
                case LLVMRealOGE: op2 = CVM_OP_FCMP_LE; swap = 1;   break;
                case LLVMRealUGE: op2 = CVM_OP_FCMP_LT; negate = 1; break; /* !(a<b)  */
                case LLVMRealUGT: op2 = CVM_OP_FCMP_LE; negate = 1; break; /* !(a<=b) */
                case LLVMRealULT: op2 = CVM_OP_FCMP_LE; swap = 1; negate = 1; break; /* !(b<=a) */
                case LLVMRealULE: op2 = CVM_OP_FCMP_LT; swap = 1; negate = 1; break; /* !(b<a)  */
                case LLVMRealORD: compound_kind = 1;                break;
                case LLVMRealUNO: compound_kind = 2;                break;
                case LLVMRealONE: compound_kind = 3;                break; /* (a<b)|(b<a)   */
                case LLVMRealUEQ: compound_kind = 4;                break; /* !((a<b)|(b<a))*/
                default: break;
                }
                if (op2 < 0 && compound_kind == 0) {
                    ERR(cg->fn_name,
                        "fcmp predicate %d not in the supported subset", (int)p);
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
                } else if (compound_kind == 3 || compound_kind == 4) {
                    /* one(a,b) = (a<b) | (b<a) ; ueq = !one */
                    uint8_t t1 = cg_alloc_reg(cg);
                    uint8_t t2 = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_FCMP_LT, t1,  lhs, rhs));
                    cg_emit(cg, enc_r(CVM_OP_FCMP_LT, t2,  rhs, lhs));
                    cg_emit(cg, enc_r(CVM_OP_OR,      dst, t1,  t2 ));
                } else {
                    if (swap) cg_emit(cg, enc_r((uint8_t)op2, dst, rhs, lhs));
                    else      cg_emit(cg, enc_r((uint8_t)op2, dst, lhs, rhs));
                }
                /* Flip the 0/1 result for the unordered-relational and UEQ
                 * predicates: dst = 1 - dst. */
                if (negate || compound_kind == 4) {
                    uint8_t one_r = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_i16(CVM_OP_MOVI, one_r, 1));
                    cg_emit(cg, enc_r(CVM_OP_SUB, dst, one_r, dst));
                }
                break;
            }

            case LLVMFPTrunc: {
                /* double -> float. Only the f64 case reaches here (f32->f32
                 * is a no-op clang never emits). Soft-float runtime call. */
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                LLVMValueRef a1[1] = { LLVMGetOperand(i, 0) };
                cg_emit_runtime_call(cg, i, "__cvm_f_to_f32", a1, 1, NULL, -1, dst, 0);
                break;
            }

            case LLVMSIToFP: case LLVMUIToFP:
            case LLVMFPToSI: case LLVMFPToUI: {
                /* f64 conversions go through the soft-float runtime. (sitofp/
                 * uitofp with an f64 RESULT are diverted before this switch;
                 * only fptosi/fptoui with an f64 SOURCE arrive here.) */
                if ((op == LLVMFPToSI || op == LLVMFPToUI) &&
                    cg_type_is_f64(LLVMTypeOf(LLVMGetOperand(i, 0)))) {
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    const char *fn = (op == LLVMFPToSI) ? "__cvm_f_to_i32"
                                                        : "__cvm_f_to_u32";
                    LLVMValueRef a1[1] = { LLVMGetOperand(i, 0) };
                    cg_emit_runtime_call(cg, i, fn, a1, 1, NULL, -1, dst, 0);
                    break;
                }
                uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                /* `sitofp iN` (N<32) on a narrow operand: the value sits
                 * zero-extended in its register (LDB/LDH zero-extend), so
                 * I2F_S would read e.g. i16 -2008 as +63528. Sign-extend it
                 * first via the SHL/SAR pair (as the SExt lowering does).
                 * UIToFP is correct as-is — zero-extension IS the unsigned
                 * value. */
                if (op == LLVMSIToFP) {
                    LLVMTypeRef sty = LLVMTypeOf(LLVMGetOperand(i, 0));
                    if (LLVMGetTypeKind(sty) == LLVMIntegerTypeKind) {
                        unsigned w = LLVMGetIntTypeWidth(sty);
                        if (w < 32) {
                            int16_t shift = (int16_t)(32u - w);
                            cg_emit(cg, enc_i16(CVM_OP_MOVI,
                                                (uint8_t)CG_REG_SCRATCH, shift));
                            cg_emit(cg, enc_r(CVM_OP_SHL, dst, src,
                                              (uint8_t)CG_REG_SCRATCH));
                            cg_emit(cg, enc_r(CVM_OP_SAR, dst, dst,
                                              (uint8_t)CG_REG_SCRATCH));
                            src = dst;   /* convert from the sign-extended reg */
                        }
                    }
                }
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
                    int true_phi  = cg_block_has_phi(true_bb);
                    int false_phi = cg_block_has_phi(false_bb);

                    /* Phi-move ISOLATION (see cg_block_has_phi): a move must run
                     * only on its own edge. Emitting both edges' moves before
                     * the branch (the naive form) lets one edge's write corrupt
                     * a value the other edge needs — e.g. a single-block loop
                     * `for(;*p;p=p->next)` whose back-edge updates the `p` phi
                     * register that the exit edge then reads.
                     *
                     * We place an edge's moves on the fall-through AFTER the
                     * conditional branch so they run only when that edge is
                     * taken. The branch-relaxation pass preserves this (it flips
                     * the opcode and keeps the +1 skip), so it survives long
                     * branches. A conditional branch can only fall through to
                     * ONE edge, so when BOTH successors carry moves we add a
                     * local skip to isolate the second. */
                    if (true_phi && false_phi) {
                        /* Both edges carry moves: fully isolate.
                         *   BNE cond -> (skip the false section)   cond!=0=true
                         *   <false moves>; JMP false_bb            cond==0=false
                         *   <true moves>;  JMP true_bb             (skip lands here) */
                        uint32_t bne_idx = cg->count;
                        cg_emit(cg, enc_br(CVM_OP_BNE, cond_reg, cg->zero_reg, 0));
                        cg_emit_phi_moves(cg, cg->cur_block, false_bb);
                        cg_queue_fixup(cg, cg->count, false_bb, 8, 24);
                        cg_emit(cg, enc_i24(CVM_OP_JMP, 0));
                        int32_t skip = (int32_t)cg->count - (int32_t)(bne_idx + 1u);
                        if (skip < -128 || skip > 127) {
                            /* This BNE carries no fixup (its target is a local
                             * offset, not a block) so relaxation can't lift it;
                             * fail loudly rather than emit a wrong skip. Only a
                             * pathological count of phi moves on the false edge
                             * could trigger this. */
                            ERR(cg->fn_name,
                                "phi-isolation skip %d out of imm8 range "
                                "(too many phi moves on one edge)", skip);
                            cg->had_error = 1;
                        } else {
                            cg->code[bne_idx] =
                                enc_br(CVM_OP_BNE, cond_reg, cg->zero_reg,
                                       (int8_t)skip);
                        }
                        cg_emit_phi_moves(cg, cg->cur_block, true_bb);
                        cg_queue_fixup(cg, cg->count, true_bb, 8, 24);
                        cg_emit(cg, enc_i24(CVM_OP_JMP, 0));
                    } else if (true_phi) {
                        /* Only the true edge has moves: branch to the move-less
                         * false edge, then run the true moves on the fall-through. */
                        cg_queue_fixup(cg, cg->count, false_bb, 24, 8);
                        cg_emit(cg, enc_br(CVM_OP_BEQ, cond_reg, cg->zero_reg, 0));
                        cg_emit_phi_moves(cg, cg->cur_block, true_bb);
                        cg_queue_fixup(cg, cg->count, true_bb, 8, 24);
                        cg_emit(cg, enc_i24(CVM_OP_JMP, 0));
                    } else {
                        /* Only the false edge has moves, or neither: branch to
                         * the move-less true edge, then run any false moves on
                         * the fall-through. (Neither-moves degenerates to the
                         * original BNE/JMP pair.) */
                        cg_queue_fixup(cg, cg->count, true_bb, 24, 8);
                        cg_emit(cg, enc_br(CVM_OP_BNE, cond_reg, cg->zero_reg, 0));
                        cg_emit_phi_moves(cg, cg->cur_block, false_bb);
                        cg_queue_fixup(cg, cg->count, false_bb, 8, 24);
                        cg_emit(cg, enc_i24(CVM_OP_JMP, 0));
                    }
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
                     * miss the 0xFF cond_reg.
                     *
                     * case_reg/tmp are dead after each case's BNE, so we
                     * snapshot next_reg here (ABOVE cond_reg, which may be a
                     * spill-reload transient that must stay live across the
                     * whole chain) and reset to it each iteration. Otherwise
                     * a switch with many cases would walk next_reg off the
                     * end of the emit scratch window. */
                    int chain_floor = cg->next_reg;
                    for (unsigned k = 1; k < n_succ; ++k) {
                        cg->next_reg = chain_floor;
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
                    cg->next_reg = chain_floor;
                    if (cg->had_error) break;

                    cg_queue_fixup(cg, cg->count, default_bb, 8, 24);
                    cg_emit(cg, enc_i24(CVM_OP_JMP, 0));
                }
                break;
            }

            case LLVMRet: {
                if (LLVMGetNumOperands(i) > 0) {
                    LLVMValueRef rv = LLVMGetOperand(i, 0);
                    if (cg_type_is_wide(LLVMTypeOf(rv))) {
                        /* 64-bit return: lo -> R0, hi -> R1. */
                        uint8_t lo, hi;
                        if (cg_i64_read(cg, rv, &lo, &hi)) break;
                        if (lo != 0) cg_emit(cg, enc_r(CVM_OP_MOV, 0, lo, 0));
                        if (hi != 1) cg_emit(cg, enc_r(CVM_OP_MOV, 1, hi, 0));
                    } else {
                        uint8_t r = cg_reg_for(cg, rv);
                        if (cg->had_error) break;
                        if (r != 0)
                            cg_emit(cg, enc_r(CVM_OP_MOV, 0, r, 0));
                    }
                }
                if (cg_sp_add(cg, cg->frame_bytes)) break;
                cg_emit(cg, enc_r(CVM_OP_RET, 0, 0, 0));
                break;
            }

            case LLVMUnreachable:
                /* LLVM guarantees control never reaches here — it follows a
                 * `noreturn` call (I_Error, abort, __assert_fail, exit) or a
                 * fully-covered switch with no default. clang -O1 emits a
                 * bare `unreachable` terminator. There is no fall-through to
                 * protect, but emit a HALT as a defensive backstop: if a
                 * mistranslated predecessor ever does reach it, the VM stops
                 * cleanly instead of executing whatever bytes follow. */
                cg_emit(cg, enc_r(CVM_OP_HALT, 0, 0, 0));
                break;

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

                /* A 64-bit store splits into two 32-bit word stores at addr
                 * and addr+4. The value is either a constant (clang lowers
                 * struct/array zero-init and small copies as 8-byte `store i64`
                 * chunks) or an i64 SSA value (legalised into frame slots);
                 * cg_i64_read materialises lo/hi for both. */
                if (cg_type_is_wide(vty)) {
                    uint8_t addr = cg_reg_for(cg, LLVMGetOperand(i, 1));
                    if (cg->had_error) break;
                    uint8_t lo, hi;
                    if (cg_i64_read(cg, val_v, &lo, &hi)) break;
                    cg_emit(cg, enc_r(CVM_OP_STW, 0, addr, lo));
                    /* high word at addr + 4 */
                    cg_movi_scratch(cg, 4);
                    uint8_t a4 = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_ADD, a4, addr, (uint8_t)CG_REG_SCRATCH));
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
            case LLVMFreeze:   /* identity for i32/f32/ptr (wide handled above) */
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
                LLVMValueRef src_v = LLVMGetOperand(i, 0);
                uint8_t dst = cg->regs[cg_lookup(cg, i)];
                LLVMTypeRef dty = LLVMTypeOf(i);
                unsigned dw = LLVMGetIntTypeWidth(dty);
                /* trunc from i64: the result is just the LO word (the high
                 * word is discarded). Load it straight into dst; a following
                 * mask handles sub-i32 destinations below. */
                if (cg_type_is_i64(LLVMTypeOf(src_v))) {
                    if (LLVMIsAConstantInt(src_v)) {
                        unsigned long long k = LLVMConstIntGetZExtValue(src_v);
                        cg_emit_load_const32(cg, dst, (int32_t)(uint32_t)k);
                    } else {
                        int sidx = cg_lookup(cg, src_v);
                        if (sidx < 0 || cg->i64_slot[sidx] == CG_NO_SLOT) {
                            ERR(cg->fn_name, "trunc: i64 source without slot");
                            cg->had_error = 1;
                            break;
                        }
                        if (cg_ldw_sp_off(cg, dst, cg_i64_lo_off(cg, sidx))) {
                            cg->had_error = 1;
                            break;
                        }
                    }
                    if (dw < 32) {
                        uint32_t mask = (dw == 0) ? 0u
                            : (uint32_t)(((uint64_t)1 << dw) - 1u);
                        cg_emit_load_const32(cg, (uint8_t)CG_REG_SCRATCH,
                                             (int32_t)mask);
                        cg_emit(cg, enc_r(CVM_OP_AND, dst, dst,
                                          (uint8_t)CG_REG_SCRATCH));
                    }
                    break;
                }
                uint8_t src = cg_reg_for(cg, src_v);
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

                /* setjmp/longjmp -> dedicated non-local-jump opcodes. The libc
                 * only DECLARES them (no body), so a normal call would fail
                 * "extern not supported"; instead we capture/restore {pc, sp,
                 * dest reg} in the jmp_buf. clang marks setjmp returns_twice, so
                 * the IR already keeps values live across it in memory. */
                if (strcmp(name, "setjmp") == 0 || strcmp(name, "_setjmp") == 0) {
                    uint8_t env = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_SETJMP, dst, env, 0));
                    break;
                }
                if (strcmp(name, "longjmp") == 0 || strcmp(name, "_longjmp") == 0) {
                    uint8_t env = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t val = cg_reg_for(cg, LLVMGetOperand(i, 1));
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r(CVM_OP_LONGJMP, env, val, 0));
                    break;
                }
                /* __cvm_coro_swap_raw(from, to) falls through to the full
                 * call-lowering protocol — which spills caller-saved SSA
                 * regs (live across the call) and lands args in R0/R1
                 * exactly like a real call. At step 4 (where CALL would
                 * be emitted) the protocol substitutes CORO_SWAP R0, R1
                 * instead, since this function has no body. Without the
                 * spill protocol, a caller's SSA reg that survives the
                 * swap would be clobbered by the callee's register usage
                 * (the VM register file is shared across coroutines —
                 * CORO_SWAP only saves/restores PC+SP+status). */

                /* llvm.abs.iN(%x, _is_int_min_poison)  (N = 8/16/32)
                 *   abs(x) = (x<0) ? -x : x
                 *
                 *   [if N<32: sign-extend x to 32 bits first]
                 *   CMP_LT cond, x, zero
                 *   SUB    neg, zero, x
                 *   BEQ    cond, zero, +2   ; x>=0 ? skip true branch
                 *   MOV    dst, neg
                 *   JMP    +1
                 *   MOV    dst, x
                 *
                 * Narrow widths (i8/i16) live in 32-bit regs possibly NOT
                 * sign-extended, so CMP_LT/SUB would interpret the value wrong
                 * (same class of bug as the sitofp-narrow miscompile). Sign-
                 * extend via SHL/SAR (as in cg_icmp_operand / the SExt lowering)
                 * before the abs. clang emits abs.i16 for e.g. UQM grpinfo.c's
                 * BuildGroups dx/dy distance math. */
                {
                    int abs_bits = (strncmp(name, "llvm.abs.i", 10) == 0)
                                       ? atoi(name + 10) : 0;
                    if (abs_bits == 8 || abs_bits == 16 || abs_bits == 32) {
                    uint8_t x    = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t dst  = cg->regs[cg_lookup(cg, i)];
                    uint8_t cond = cg_alloc_reg(cg);
                    uint8_t neg  = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    if (abs_bits < 32) {
                        uint8_t sx = cg_alloc_reg(cg);
                        int16_t shift = (int16_t)(32u - (unsigned)abs_bits);
                        if (cg->had_error) break;
                        cg_emit(cg, enc_i16(CVM_OP_MOVI, (uint8_t)CG_REG_SCRATCH, shift));
                        cg_emit(cg, enc_r(CVM_OP_SHL, sx, x, (uint8_t)CG_REG_SCRATCH));
                        cg_emit(cg, enc_r(CVM_OP_SAR, sx, sx, (uint8_t)CG_REG_SCRATCH));
                        x = sx;
                    }
                    cg_emit(cg, enc_r (CVM_OP_CMP_LT, cond, x, cg->zero_reg));
                    cg_emit(cg, enc_r (CVM_OP_SUB, neg, cg->zero_reg, x));
                    cg_emit(cg, enc_br(CVM_OP_BEQ, cond, cg->zero_reg, 2));
                    cg_emit(cg, enc_r (CVM_OP_MOV, dst, neg, 0));
                    cg_emit(cg, enc_i24(CVM_OP_JMP, 1));
                    cg_emit(cg, enc_r (CVM_OP_MOV, dst, x, 0));
                    break;
                    }
                }

                /* llvm.ctlz.i32(%x, is_zero_undef) — count leading zeros.
                 * No CLZ opcode, so lower to a tiny loop: shift x right until
                 * it is zero, counting shifts (= highest-set-bit index + 1);
                 * the result is 32 - count, which is also correct for x==0
                 * (0 shifts -> 32). The is_zero_undef flag doesn't affect this
                 * exact lowering. clang folds bit-scan idioms (R_FixWiggle's
                 * log2) into this intrinsic. */
                if (strcmp(name, "llvm.ctlz.i32") == 0) {
                    uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    uint8_t x   = cg_alloc_reg(cg);
                    uint8_t n   = cg_alloc_reg(cg);
                    uint8_t one = cg_alloc_reg(cg);
                    uint8_t k32 = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r  (CVM_OP_MOV,  x, src, 0));
                    cg_emit(cg, enc_i16(CVM_OP_MOVI, n, 0));
                    cg_emit(cg, enc_i16(CVM_OP_MOVI, one, 1));
                    /* loop: while (x != 0) { x >>= 1; n++; } */
                    cg_emit(cg, enc_br (CVM_OP_BEQ, x, cg->zero_reg, 3)); /* x==0 -> exit */
                    cg_emit(cg, enc_r  (CVM_OP_SHR, x, x, one));          /* x >>= 1   */
                    cg_emit(cg, enc_r  (CVM_OP_ADD, n, n, one));          /* n++       */
                    cg_emit(cg, enc_i24(CVM_OP_JMP, -4));                 /* loop back */
                    cg_emit(cg, enc_i16(CVM_OP_MOVI, k32, 32));
                    cg_emit(cg, enc_r  (CVM_OP_SUB, dst, k32, n));        /* 32 - n    */
                    break;
                }

                /* llvm.ctpop.iN(%x) — population count (set bits). No POPCNT
                 * opcode, so lower with Kernighan's loop (one iteration per set
                 * bit): n=0; while (x) { x &= x-1; n++; }. clang folds power-of-
                 * two tests `x & (x-1)` into a ctpop compare, so this shows up
                 * in ordinary code (e.g. DOOM's texture-height pow2 check, and
                 * stb_image's PNG depth handling for the i8 form). Narrow widths
                 * (i8/i16) live in 32-bit regs possibly sign-extended, so mask
                 * to the value width first or phantom high bits get counted. */
                {
                    uint32_t cpop_mask = 0;
                    int cpop_match = 0;
                    if (strcmp(name, "llvm.ctpop.i32") == 0) { cpop_match = 1; cpop_mask = 0; }
                    else if (strcmp(name, "llvm.ctpop.i16") == 0) { cpop_match = 1; cpop_mask = 0xFFFF; }
                    else if (strcmp(name, "llvm.ctpop.i8") == 0)  { cpop_match = 1; cpop_mask = 0xFF; }
                if (cpop_match) {
                    uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    uint8_t x   = cg_alloc_reg(cg);
                    uint8_t n   = cg_alloc_reg(cg);
                    uint8_t one = cg_alloc_reg(cg);
                    uint8_t t   = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r  (CVM_OP_MOV,  x, src, 0));
                    if (cpop_mask) {
                        uint8_t m = cg_alloc_reg(cg);
                        if (cg->had_error) break;
                        cg_emit_load_const32(cg, m, (int32_t)cpop_mask);
                        cg_emit(cg, enc_r(CVM_OP_AND, x, x, m));  /* mask to width */
                    }
                    cg_emit(cg, enc_i16(CVM_OP_MOVI, n, 0));
                    cg_emit(cg, enc_i16(CVM_OP_MOVI, one, 1));
                    /* loop: while (x != 0) { x &= x-1; n++; } */
                    cg_emit(cg, enc_br (CVM_OP_BEQ, x, cg->zero_reg, 4)); /* x==0 -> exit */
                    cg_emit(cg, enc_r  (CVM_OP_SUB, t, x, one));          /* t = x-1   */
                    cg_emit(cg, enc_r  (CVM_OP_AND, x, x, t));            /* x &= x-1  */
                    cg_emit(cg, enc_r  (CVM_OP_ADD, n, n, one));          /* n++       */
                    cg_emit(cg, enc_i24(CVM_OP_JMP, -5));                 /* loop back */
                    cg_emit(cg, enc_r  (CVM_OP_MOV, dst, n, 0));          /* dst = n   */
                    break;
                }
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

                /* llvm.fabs.f32(x) = clear the sign bit. Floats share the
                 * integer register file (bitcast f32<->i32 is free), so fabs is
                 * just AND with 0x7FFFFFFF. clang emits it from fabsf/fabs on
                 * float (e.g. the SDK math.h fabsf). */
                if (strcmp(name, "llvm.fabs.f32") == 0) {
                    uint8_t x    = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t dst  = cg->regs[cg_lookup(cg, i)];
                    uint8_t mask = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit_load_const32(cg, mask, 0x7FFFFFFF);
                    cg_emit(cg, enc_r(CVM_OP_AND, dst, x, mask));
                    break;
                }

                /* llvm.copysign.f32(x, y) = (x & 0x7FFFFFFF) | (y & 0x80000000) */
                if (strcmp(name, "llvm.copysign.f32") == 0) {
                    uint8_t x     = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t y     = cg_reg_for(cg, LLVMGetOperand(i, 1));
                    uint8_t dst   = cg->regs[cg_lookup(cg, i)];
                    uint8_t mabs  = cg_alloc_reg(cg);
                    uint8_t msgn  = cg_alloc_reg(cg);
                    uint8_t tx    = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit_load_const32(cg, mabs, 0x7FFFFFFF);
                    cg_emit_load_const32(cg, msgn, (int32_t)0x80000000);
                    cg_emit(cg, enc_r(CVM_OP_AND, tx,  x, mabs));
                    cg_emit(cg, enc_r(CVM_OP_AND, dst, y, msgn));
                    cg_emit(cg, enc_r(CVM_OP_OR,  dst, dst, tx));
                    break;
                }

                /* llvm.bswap.i16(x) = (x<<8 & 0xFF00) | (x>>8 & 0x00FF).
                 * clang folds the BigShort/LittleShort byte-swap idioms (PAK/WAD
                 * big-endian fields) into this; lower with shift+mask+or so the
                 * engine's byte swaps work without a volatile-array workaround. */
                if (strcmp(name, "llvm.bswap.i16") == 0) {
                    uint8_t x   = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    uint8_t t1  = cg_alloc_reg(cg);
                    uint8_t t2  = cg_alloc_reg(cg);
                    uint8_t k8  = cg_alloc_reg(cg);
                    uint8_t kff = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit_load_const32(cg, k8, 8);
                    cg_emit_load_const32(cg, kff, 0xFF);
                    cg_emit(cg, enc_r(CVM_OP_AND, t1, x, kff));   /* t1 = x & 0xFF      */
                    cg_emit(cg, enc_r(CVM_OP_SHL, t1, t1, k8));   /* t1 <<= 8           */
                    cg_emit(cg, enc_r(CVM_OP_SHR, t2, x, k8));    /* t2 = (u)x >> 8     */
                    cg_emit(cg, enc_r(CVM_OP_AND, t2, t2, kff));  /* t2 &= 0xFF         */
                    cg_emit(cg, enc_r(CVM_OP_OR,  dst, t1, t2));
                    break;
                }

                /* llvm.bswap.i32(x) = full 4-byte reverse.
                 *   (x>>24) | ((x>>8)&0xFF00) | ((x<<8)&0xFF0000) | (x<<24) */
                if (strcmp(name, "llvm.bswap.i32") == 0) {
                    uint8_t x    = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t dst  = cg->regs[cg_lookup(cg, i)];
                    uint8_t b0   = cg_alloc_reg(cg);
                    uint8_t b1   = cg_alloc_reg(cg);
                    uint8_t b2   = cg_alloc_reg(cg);
                    uint8_t b3   = cg_alloc_reg(cg);
                    uint8_t k8   = cg_alloc_reg(cg);
                    uint8_t k24  = cg_alloc_reg(cg);
                    uint8_t m1   = cg_alloc_reg(cg);
                    uint8_t m2   = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit_load_const32(cg, k8, 8);
                    cg_emit_load_const32(cg, k24, 24);
                    cg_emit_load_const32(cg, m1, 0x0000FF00);
                    cg_emit_load_const32(cg, m2, 0x00FF0000);
                    cg_emit(cg, enc_r(CVM_OP_SHR, b0, x, k24));   /* b0 = (u)x>>24 (low byte) */
                    cg_emit(cg, enc_r(CVM_OP_SHR, b1, x, k8));    /* b1 = (u)x>>8             */
                    cg_emit(cg, enc_r(CVM_OP_AND, b1, b1, m1));   /* b1 &= 0xFF00             */
                    cg_emit(cg, enc_r(CVM_OP_SHL, b2, x, k8));    /* b2 = x<<8                */
                    cg_emit(cg, enc_r(CVM_OP_AND, b2, b2, m2));   /* b2 &= 0xFF0000           */
                    cg_emit(cg, enc_r(CVM_OP_SHL, b3, x, k24));   /* b3 = x<<24 (top byte)    */
                    cg_emit(cg, enc_r(CVM_OP_OR,  dst, b0, b1));
                    cg_emit(cg, enc_r(CVM_OP_OR,  dst, dst, b2));
                    cg_emit(cg, enc_r(CVM_OP_OR,  dst, dst, b3));
                    break;
                }

                /* llvm.bitreverse.i8/i16/i32(x) — reverse the N bit positions.
                 * clang folds hand-written bit-reversal idioms into this (e.g.
                 * stb_image's stbi__bitreverse16 in the zlib/PNG decoder). Lower
                 * with the classic swap network — for i16:
                 *   x = ((x&0xAAAA)>>1)|((x&0x5555)<<1);
                 *   x = ((x&0xCCCC)>>2)|((x&0x3333)<<2);
                 *   x = ((x&0xF0F0)>>4)|((x&0x0F0F)<<4);
                 *   x = ((x&0xFF00)>>8)|((x&0x00FF)<<8);
                 * Generalised to any width N (8/16/32): one step per shift
                 * s = 1,2,4,...,N/2, with the alternating lo/hi masks computed
                 * for N bits. Source is masked to N bits first (narrow values
                 * may carry sign-extended high bits in the 32-bit reg). */
                {
                    int br_bits = 0;
                    if      (strcmp(name, "llvm.bitreverse.i8")  == 0) br_bits = 8;
                    else if (strcmp(name, "llvm.bitreverse.i16") == 0) br_bits = 16;
                    else if (strcmp(name, "llvm.bitreverse.i32") == 0) br_bits = 32;
                    if (br_bits) {
                    uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    uint8_t x   = cg_alloc_reg(cg);
                    uint8_t t   = cg_alloc_reg(cg);
                    uint8_t mh  = cg_alloc_reg(cg);
                    uint8_t ml  = cg_alloc_reg(cg);
                    uint8_t sh  = cg_alloc_reg(cg);
                    uint8_t kw  = cg_alloc_reg(cg);
                    int s;
                    uint32_t width_mask = (br_bits == 32)
                        ? 0xFFFFFFFFu : ((1u << br_bits) - 1u);
                    if (cg->had_error) break;
                    cg_emit_load_const32(cg, kw, (int32_t)width_mask);
                    cg_emit(cg, enc_r(CVM_OP_AND, x, src, kw)); /* x = src & widthmask */
                    for (s = 1; s < br_bits; s <<= 1) {
                        /* lo mask: low s bits set in each 2s-bit group, over N bits */
                        uint32_t lo = 0;
                        int bit;
                        for (bit = 0; bit < br_bits; ++bit)
                            if ((bit % (2 * s)) < s) lo |= (1u << bit);
                        uint32_t hi = (lo << s) & width_mask;
                        cg_emit_load_const32(cg, mh, (int32_t)hi);
                        cg_emit_load_const32(cg, ml, (int32_t)lo);
                        cg_emit_load_const32(cg, sh, s);
                        cg_emit(cg, enc_r(CVM_OP_AND, t, x, mh));  /* t = x & hi  */
                        cg_emit(cg, enc_r(CVM_OP_SHR, t, t, sh));  /* t >>= s     */
                        cg_emit(cg, enc_r(CVM_OP_AND, x, x, ml));  /* x &= lo     */
                        cg_emit(cg, enc_r(CVM_OP_SHL, x, x, sh));  /* x <<= s     */
                        cg_emit(cg, enc_r(CVM_OP_OR,  x, x, t));   /* x = x | t   */
                    }
                    cg_emit(cg, enc_r(CVM_OP_MOV, dst, x, 0));
                    break;
                    }
                }

                /* llvm.cttz.i32(%x, is_zero_undef) — count trailing zeros. No
                 * CTTZ opcode; lower to a loop counting low zero bits (result
                 * 32 for x==0, matching the is_zero_undef=false semantics).
                 * clang folds power-of-two log2 idioms (e.g. the renderer's
                 * MaskForNum) into this. Mirrors the ctlz loop. */
                if (strcmp(name, "llvm.cttz.i32") == 0) {
                    uint8_t src = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    uint8_t x   = cg_alloc_reg(cg);
                    uint8_t n   = cg_alloc_reg(cg);
                    uint8_t one = cg_alloc_reg(cg);
                    uint8_t t   = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    cg_emit(cg, enc_r  (CVM_OP_MOV,  x, src, 0));     /* 0 */
                    cg_emit(cg, enc_i16(CVM_OP_MOVI, n, 0));          /* 1 */
                    cg_emit(cg, enc_i16(CVM_OP_MOVI, one, 1));        /* 2 */
                    cg_emit(cg, enc_br (CVM_OP_BEQ, x, cg->zero_reg, 5)); /* 3: x==0 -> n=32 */
                    cg_emit(cg, enc_r  (CVM_OP_AND, t, x, one));      /* 4: loop top */
                    cg_emit(cg, enc_br (CVM_OP_BNE, t, cg->zero_reg, 4)); /* 5: low bit set -> done */
                    cg_emit(cg, enc_r  (CVM_OP_SHR, x, x, one));      /* 6 */
                    cg_emit(cg, enc_r  (CVM_OP_ADD, n, n, one));      /* 7 */
                    cg_emit(cg, enc_i24(CVM_OP_JMP, -5));             /* 8: back to 4 */
                    cg_emit(cg, enc_i16(CVM_OP_MOVI, n, 32));         /* 9: x==0 case */
                    cg_emit(cg, enc_r  (CVM_OP_MOV, dst, n, 0));      /* 10: done */
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
                if (strncmp(name, "llvm.fshl.i", 11) == 0 ||
                    strncmp(name, "llvm.fshr.i", 11) == 0)
                {
                    /* "llvm.fshl.iN" vs "llvm.fshr.iN" differ at index 8
                     * (the 'l' / 'r'); index 6 is 's' in both. The bit
                     * width N follows the trailing 'i'. */
                    int is_fshr = (name[8] == 'r');
                    int fbits   = atoi(name + 11);
                    if (LLVMGetNumArgOperands(i) != 3) {
                        ERR(cg->fn_name, "%s expects 3 args, got %u", name,
                            LLVMGetNumArgOperands(i));
                        cg->had_error = 1;
                        break;
                    }
                    if (fbits <= 0 || fbits > 32) {
                        ERR(cg->fn_name, "%s width %d unsupported", name, fbits);
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
                    if (cg->had_error) break;

                    /* Shift amount is reduced modulo the funnel width N.
                     * For N==32 the VM's SHL/SHR already mask to low 5 bits;
                     * for narrow N we explicitly AND with (N-1) (N is a
                     * power of two for every width clang emits: i8/i16). */
                    uint8_t s = c;
                    if (fbits < 32) {
                        s = cg_alloc_reg(cg);
                        if (cg->had_error) break;
                        cg_movi_scratch(cg, fbits - 1);
                        cg_emit(cg, enc_r(CVM_OP_AND, s, c,
                                          (uint8_t)CG_REG_SCRATCH));
                    }

                    /* Forward-shift first (a<<s for fshl, b>>s for fshr). */
                    if (is_fshr) cg_emit(cg, enc_r(CVM_OP_SHR, b_sh, b, s));
                    else         cg_emit(cg, enc_r(CVM_OP_SHL, a_sh, a, s));
                    /* inv = N - s. Materialise N in scratch, then SUB. */
                    cg_movi_scratch(cg, fbits);
                    cg_emit(cg, enc_r(CVM_OP_SUB, inv,
                                      (uint8_t)CG_REG_SCRATCH, s));
                    /* Inverse shift on the other operand. For narrow widths
                     * b must be confined to its low N bits first so the
                     * right-shift doesn't pull in stale high garbage. */
                    if (is_fshr) {
                        cg_emit(cg, enc_r(CVM_OP_SHL, a_sh, a, inv));
                    } else {
                        if (fbits < 32) {
                            cg_movi_scratch(cg,
                                (int32_t)(((uint32_t)1 << fbits) - 1u));
                            cg_emit(cg, enc_r(CVM_OP_AND, b_sh, b,
                                              (uint8_t)CG_REG_SCRATCH));
                            cg_emit(cg, enc_r(CVM_OP_SHR, b_sh, b_sh, inv));
                        } else {
                            cg_emit(cg, enc_r(CVM_OP_SHR, b_sh, b, inv));
                        }
                    }
                    cg_emit(cg, enc_r(CVM_OP_OR, dst, a_sh, b_sh));
                    /* Narrow result: confine to low N bits. */
                    if (fbits < 32) {
                        cg_movi_scratch(cg,
                            (int32_t)(((uint32_t)1 << fbits) - 1u));
                        cg_emit(cg, enc_r(CVM_OP_AND, dst, dst,
                                          (uint8_t)CG_REG_SCRATCH));
                    }
                    /* Fixup for s==0: result is the identity operand
                     * (a for fshl, b for fshr) because the inverse shift by
                     * N would be a full-width shift the VM can't express. */
                    cg_emit(cg, enc_br(CVM_OP_BNE, s, cg->zero_reg, 1));
                    cg_emit(cg, enc_r(CVM_OP_MOV, dst,
                                      is_fshr ? b : a, 0));
                    break;
                }

                /* `cvm_intrin_*` extern declarations from the runtime
                 * intrinsics header lower to a single opcode instead of a
                 * CALL — there's no body anywhere; the name exists only
                 * so clang has something to reference in the IR. */
                {
                    int is_mulh    = (strcmp(name, "cvm_intrin_mulh")        == 0);
                    int is_mulhu   = (strcmp(name, "cvm_intrin_mulhu")       == 0);
                    int is_qdiv    = (strcmp(name, "cvm_intrin_qdiv_16_16")  == 0);
                    int is_f2i_s   = (strcmp(name, "cvm_intrin_f2i_sat_s")   == 0);
                    int is_f2i_u   = (strcmp(name, "cvm_intrin_f2i_sat_u")   == 0);
                    int is_fsqrt   = (strcmp(name, "cvm_intrin_fsqrt")       == 0);

                    int two_arg_op = -1;   /* MULH/MULHU/QDIV1616 shape */
                    int one_arg_op = -1;   /* F2I_* / FSQRT shape */
                    if (is_mulh)       two_arg_op = CVM_OP_MULH;
                    else if (is_mulhu) two_arg_op = CVM_OP_MULHU;
                    else if (is_qdiv)  two_arg_op = CVM_OP_QDIV1616;
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

                    /* General 64/32 divide: cvm_intrin_qdiv_64_32(hi, lo, div).
                     * QDIV6432 ties the dividend-high to the destination reg, so
                     * a 3-source/1-dest op fits the 3-register encoding. We stage
                     * through a fresh temp (MOV hi -> tmp; QDIV6432 tmp,lo,div;
                     * MOV tmp -> dst) so the divide can't be corrupted by dst
                     * aliasing the lo/divisor operands. */
                    if (strcmp(name, "cvm_intrin_qdiv_64_32") == 0) {
                        if (LLVMGetNumArgOperands(i) != 3) {
                            ERR(cg->fn_name, "%s expects 3 args, got %u", name,
                                LLVMGetNumArgOperands(i));
                            cg->had_error = 1;
                            break;
                        }
                        uint8_t r_hi = cg_reg_for(cg, LLVMGetOperand(i, 0));
                        uint8_t r_lo = cg_reg_for(cg, LLVMGetOperand(i, 1));
                        uint8_t r_dv = cg_reg_for(cg, LLVMGetOperand(i, 2));
                        uint8_t dst  = cg->regs[cg_lookup(cg, i)];
                        uint8_t tmp  = cg_alloc_reg(cg);
                        if (cg->had_error) break;
                        cg_emit(cg, enc_r(CVM_OP_MOV, tmp, r_hi, 0));
                        cg_emit(cg, enc_r(CVM_OP_QDIV6432, tmp, r_lo, r_dv));
                        cg_emit(cg, enc_r(CVM_OP_MOV, dst, tmp, 0));
                        break;
                    }
                }

                /* min/max family, any integer width. clang -O1 folds
                 * `a<b?a:b` / `a>b?a:b` (and abs) into llvm.{s,u}{max,min}.iN.
                 * Lowered as cmp + branch + 2 MOVs. For narrow widths (i16/i8)
                 * the operands are extended to 32 bits before the compare so
                 * the ordering is correct; the SELECTED value is the original
                 * (untruncated) operand — the consumer truncates to width. */
                int is_min = 0, is_signed = 0, is_minmax = 0, mm_bits = 32;
                if      (strncmp(name, "llvm.smax.i", 11) == 0) { is_minmax=1; is_signed=1;            mm_bits=atoi(name+11); }
                else if (strncmp(name, "llvm.smin.i", 11) == 0) { is_minmax=1; is_signed=1; is_min=1; mm_bits=atoi(name+11); }
                else if (strncmp(name, "llvm.umax.i", 11) == 0) { is_minmax=1;                         mm_bits=atoi(name+11); }
                else if (strncmp(name, "llvm.umin.i", 11) == 0) { is_minmax=1;             is_min=1;   mm_bits=atoi(name+11); }

                if (is_minmax) {
                    uint8_t a    = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t b2   = cg_reg_for(cg, LLVMGetOperand(i, 1));
                    uint8_t dst  = cg->regs[cg_lookup(cg, i)];
                    /* compare operands ca/cb — widened for narrow types */
                    uint8_t ca = a, cb = b2;
                    if (mm_bits > 0 && mm_bits < 32) {
                        ca = cg_alloc_reg(cg);
                        cb = cg_alloc_reg(cg);
                        if (cg->had_error) break;
                        if (is_signed) {
                            uint8_t sh = cg_alloc_reg(cg);
                            if (cg->had_error) break;
                            cg_emit_load_const32(cg, sh, 32 - mm_bits);
                            cg_emit(cg, enc_r(CVM_OP_SHL, ca, a,  sh));
                            cg_emit(cg, enc_r(CVM_OP_SAR, ca, ca, sh));
                            cg_emit(cg, enc_r(CVM_OP_SHL, cb, b2, sh));
                            cg_emit(cg, enc_r(CVM_OP_SAR, cb, cb, sh));
                        } else {
                            uint8_t mask = cg_alloc_reg(cg);
                            if (cg->had_error) break;
                            cg_emit_load_const32(cg, mask, (int32_t)(((uint32_t)1 << mm_bits) - 1u));
                            cg_emit(cg, enc_r(CVM_OP_AND, ca, a,  mask));
                            cg_emit(cg, enc_r(CVM_OP_AND, cb, b2, mask));
                        }
                    }
                    uint8_t cond = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    /* cond = (ca < cb). For max, true -> b ; false -> a.
                     * For min, true -> a ; false -> b. */
                    uint8_t cmp_op = is_signed ? CVM_OP_CMP_LT : CVM_OP_CMP_LTU;
                    uint8_t true_v  = is_min ? a  : b2;
                    uint8_t false_v = is_min ? b2 : a;
                    cg_emit(cg, enc_r (cmp_op, cond, ca, cb));
                    cg_emit(cg, enc_br(CVM_OP_BEQ, cond, cg->zero_reg, 2));
                    cg_emit(cg, enc_r (CVM_OP_MOV, dst, true_v, 0));
                    cg_emit(cg, enc_i24(CVM_OP_JMP, 1));
                    cg_emit(cg, enc_r (CVM_OP_MOV, dst, false_v, 0));
                    break;
                }

                /* Saturating add/sub, any integer width. clang -O folds
                 * counters guarded with `if (x > 0) --x;` / `if (x < N) ++x;`
                 * into llvm.{u,s}{add,sub}.sat.iN. Lowered as cmp + select.
                 * NARROW WIDTHS MATTER for the add form: the upper clamp is
                 * 2^N-1 (not 0xFFFFFFFF) and the overflow test is "sum > 2^N-1"
                 * (the 32-bit carry never fires for N<32). UQM emits
                 * uadd.sat.i8 + usub.sat.i8/i16. */
                int is_sat = 0, sat_sub = 0, sat_signed = 0, sat_bits = 0;
                if      (strncmp(name, "llvm.uadd.sat.i", 15) == 0) { is_sat=1;               sat_bits=atoi(name+15); }
                else if (strncmp(name, "llvm.usub.sat.i", 15) == 0) { is_sat=1; sat_sub=1;    sat_bits=atoi(name+15); }
                else if (strncmp(name, "llvm.sadd.sat.i", 15) == 0) { is_sat=1; sat_signed=1; sat_bits=atoi(name+15); }
                else if (strncmp(name, "llvm.ssub.sat.i", 15) == 0) { is_sat=1; sat_sub=1; sat_signed=1; sat_bits=atoi(name+15); }
                if (is_sat) {
                    /* For now handle unsigned only — the signed variants
                     * have asymmetric saturation (negative-overflow → MIN,
                     * positive → MAX) which needs more instructions. UQM
                     * only emits the unsigned forms in the path we care
                     * about; reject signed loudly to surface any future
                     * need. */
                    if (sat_signed) {
                        ERR(cg->fn_name, "llvm.{s}{add,sub}.sat not yet lowered (signed)");
                        cg->had_error = 1;
                        break;
                    }
                    uint8_t a   = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    uint8_t b2  = cg_reg_for(cg, LLVMGetOperand(i, 1));
                    uint8_t dst = cg->regs[cg_lookup(cg, i)];
                    if (cg->had_error) break;
                    uint8_t tmp  = cg_alloc_reg(cg);
                    uint8_t cond = cg_alloc_reg(cg);
                    if (cg->had_error) break;
                    if (sat_sub) {
                        /* usub.sat(a, b) = (a >= b) ? (a - b) : 0
                         * Compute tmp = a - b unconditionally (cheap), then
                         * select 0 when a < b. The subtract wraps cleanly
                         * mod 2^32 either way; we just discard the wrap. */
                        cg_emit(cg, enc_r(CVM_OP_SUB, tmp, a, b2));
                        cg_emit(cg, enc_r(CVM_OP_CMP_LTU, cond, a, b2));
                        cg_emit(cg, enc_br(CVM_OP_BEQ, cond, cg->zero_reg, 2));
                        cg_emit(cg, enc_r (CVM_OP_MOV, dst, cg->zero_reg, 0));
                        cg_emit(cg, enc_i24(CVM_OP_JMP, 1));
                        cg_emit(cg, enc_r (CVM_OP_MOV, dst, tmp, 0));
                    } else {
                        /* uadd.sat(a, b): clamp to the width's max.
                         *  N==32: sum = a+b; saturate on carry (sum < a) -> 2^32-1.
                         *  N<32 : sum = a+b can't wrap a 32-bit reg (inputs are
                         *         zero-extended to <=2^N-1), so the carry test
                         *         never fires — instead saturate when sum > 2^N-1,
                         *         clamping to 2^N-1 (NOT 0xFFFFFFFF). */
                        uint8_t max_r = cg_alloc_reg(cg);
                        if (cg->had_error) break;
                        cg_emit(cg, enc_r(CVM_OP_ADD, tmp, a, b2));
                        if (sat_bits > 0 && sat_bits < 32) {
                            uint32_t maxn = ((uint32_t)1 << sat_bits) - 1u;
                            cg_emit_load_const32(cg, max_r, (int32_t)maxn);
                            /* cond = (maxn < sum)  ==  (sum > maxn) */
                            cg_emit(cg, enc_r(CVM_OP_CMP_LTU, cond, max_r, tmp));
                        } else {
                            cg_emit_load_const32(cg, max_r, -1);   /* 0xFFFFFFFF */
                            cg_emit(cg, enc_r(CVM_OP_CMP_LTU, cond, tmp, a));
                        }
                        cg_emit(cg, enc_br(CVM_OP_BEQ, cond, cg->zero_reg, 2));
                        cg_emit(cg, enc_r (CVM_OP_MOV, dst, max_r, 0));
                        cg_emit(cg, enc_i24(CVM_OP_JMP, 1));
                        cg_emit(cg, enc_r (CVM_OP_MOV, dst, tmp, 0));
                    }
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

                /* Optimiser hints with no runtime effect. `llvm.assume(i1)`
                 * (and `llvm.donothing`) carry no semantics for codegen — newer
                 * clang emits llvm.assume on noreturn/unreachable paths (e.g.
                 * the `unreachable` fixture's assert-style helper). Drop them. */
                if (strncmp(name, "llvm.assume", 11) == 0 ||
                    strncmp(name, "llvm.donothing", 14) == 0) {
                    break;
                }

                /* Alias-analysis scope metadata declarations — pure hints with
                 * no runtime effect. clang emits llvm.experimental.noalias.
                 * scope.decl when it inlines functions with restrict/noalias
                 * args (e.g. the soft int64/float64 runtime helpers). Drop. */
                if (strncmp(name, "llvm.experimental.noalias.scope.decl",
                            36) == 0) {
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
                int is_coro_swap = (name && strcmp(name, "__cvm_coro_swap_raw") == 0);

                if (!is_indirect && !is_coro_swap) {
                    callee_idx = cg_func_lookup(cg, callee_fn);
                    if (callee_idx < 0) {
                        if (name && strncmp(name, "llvm.", 5) == 0)
                            ERR(cg->fn_name,
                                "unsupported intrinsic '%s': no lowering for this "
                                "target. Rewrite the source to avoid it (e.g. take "
                                "min/max/abs/fabs out-of-line so clang can't fold "
                                "them into an intrinsic)", name);
                        else
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
                /* Word-based layout: a 64-bit arg takes two words. For
                 * scalar-only calls total_words == narg, so this reduces to
                 * the old per-arg scheme exactly. */
                unsigned total_words = 0;
                for (unsigned a = 0; a < narg; ++a)
                    total_words += (unsigned)cg_arg_words(
                                       LLVMTypeOf(LLVMGetOperand(i, a)));
                unsigned n_reg_words = call_is_vararg ? 0u
                                     : (total_words < 8 ? total_words : 8);
                unsigned n_stacked_words = total_words - n_reg_words;

                /* Argument lowering is done LAST (after caller-save spill and
                 * SP adjustment), one argument at a time, so we never need to
                 * hold many materialised/reloaded args in the emit scratch
                 * window simultaneously. A spilled SSA argument is loaded
                 * straight from its frame slot into the destination (R0..R7,
                 * or a stacked-arg slot), NOT routed through a shared reload
                 * register — that would overflow the window for a call with
                 * many spilled args (e.g. DOOM's P_TouchSpecialThing). See
                 * step 3 below. We only check the arg-count cap here. */
                if (narg > 256) {
                    ERR(cg->fn_name,
                        "call has %u args; codegen cap is 256", narg);
                    cg->had_error = 1;
                    break;
                }

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

                /* SP drops by this many bytes for stacked-arg words, so a
                 * value-spill slot at frame offset `off` is reachable at
                 * [SP_now + sp_bias + off] during the rest of the sequence.
                 * (The caller-save spill above ran BEFORE this drop, so it
                 * used the un-biased offsets — correct.) */
                int32_t sp_bias = (int32_t)(n_stacked_words * 4u);

                /* 2+3. Drop SP for the stacked words, then place every
                 *      argument WORD at its calling-convention position
                 *      (R0..R7 for words < n_reg_words, else the stack). A
                 *      64-bit arg contributes two words (lo, hi). Register and
                 *      stack placement may interleave safely: reg-arg sources
                 *      are never R0..R7 and stack stores touch memory, so
                 *      neither clobbers the other. cg_place_arg_word recycles
                 *      the emit-scratch window per word. */
                if (n_stacked_words > 0) {
                    if (cg_sp_sub(cg, n_stacked_words * 4u)) break;
                }
                {
                    unsigned wp = 0;
                    for (unsigned a = 0; a < narg && !cg->had_error; ++a) {
                        LLVMValueRef av = LLVMGetOperand(i, a);
                        if (cg_type_is_wide(LLVMTypeOf(av))) {
                            cg_place_arg_word(cg, av, 0, wp,     n_reg_words, sp_bias);
                            cg_place_arg_word(cg, av, 1, wp + 1, n_reg_words, sp_bias);
                            wp += 2;
                        } else {
                            cg_place_arg_word(cg, av, -1, wp, n_reg_words, sp_bias);
                            wp += 1;
                        }
                    }
                    if (cg->had_error) break;
                }

                /* 4. CALL or CALLR. User functions occupy FUNCS[1..N]
                 *    (index 0 is reserved as the null-fn-ptr trap), so a
                 *    direct call uses (callee_idx + 1) as the imm24. For an
                 *    indirect call, reload the (possibly spilled) callee
                 *    index here, just before CALLR, so it doesn't tie up a
                 *    register across the arg lowering. */
                if (is_indirect) {
                    int cidx = cg_lookup(cg, callee);
                    if (cidx >= 0 && cg->val_slot[cidx] != CG_NO_SLOT) {
                        /* spilled callee: reload from its SP-biased slot */
                        callee_reg = cg_alloc_reg(cg);
                        if (cg->had_error) break;
                        int32_t voff = cg_val_slot_off(cg,
                                          cg->val_slot[cidx]) + sp_bias;
                        if (cg_ldw_sp_off(cg, callee_reg, voff)) break;
                    } else {
                        callee_reg = cg_reg_for(cg, callee);
                        if (cg->had_error) break;
                    }
                    cg_emit(cg, enc_r(CVM_OP_CALLR, callee_reg, 0, 0));
                } else if (is_coro_swap) {
                    /* args landed in R0 (from) and R1 (to) via the standard
                     * arg-placement step above. CORO_SWAP saves current as
                     * SUSPENDED into R0's pointee, restores R1's pointee. */
                    cg_emit(cg, enc_r(CVM_OP_CORO_SWAP, 0, 1, 0));
                } else {
                    cg_emit(cg, enc_i24(CVM_OP_CALL, callee_idx + 1));
                }
                cg->has_calls = 1;

                /* 5. Pop stacked-arg words (caller cleans). */
                if (n_stacked_words > 0) {
                    if (cg_sp_add(cg, n_stacked_words * 4u)) break;
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

                /* 7. Take the return value (after restore so it isn't
                 *    clobbered). A 64-bit return arrives in R0:R1 (lo:hi) and
                 *    is stored to the result's two frame slots; a scalar
                 *    return is MOV'd from R0 into its SSA home. */
                LLVMTypeRef rty = LLVMTypeOf(i);
                if (cg_type_is_wide(rty)) {
                    int ridx = cg_lookup(cg, i);
                    int32_t off = cg_i64_lo_off(cg, ridx);
                    if (cg_stw_sp_off(cg, off,     0)) break;
                    if (cg_stw_sp_off(cg, off + 4, 1)) break;
                } else if (LLVMGetTypeKind(rty) != LLVMVoidTypeKind) {
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

            /* Spilled-DEF store: the handler wrote the result into def_reg
             * (the transient we redirected cg->regs[result_idx] to). Persist
             * it to the value's frame slot, then restore the spilled marker so
             * later uses route through cg_reload_spilled. SP is at its normal
             * frame position here (CALL/alloca sequences restore it before the
             * instruction ends), so the slot offset is correct. */
            if (result_spilled && !cg->had_error) {
                int32_t off = cg_val_slot_off(cg,
                                              cg->val_slot[result_idx]);
                if (cg_stw_sp_off(cg, off, def_reg)) cg->had_error = 1;
                cg->regs[result_idx] = (uint8_t)CG_REG_SPILLED;
            } else if (result_spilled) {
                cg->regs[result_idx] = (uint8_t)CG_REG_SPILLED;
            }
        }
    }

    g_cur_inst = NULL;   /* leaving per-instruction codegen; later passes have no source loc */

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

/* CRC-32 (IEEE) of a file's first `n` bytes, read in chunks. */
static int crc32_file_prefix(const char *path, size_t n, uint32_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    uint32_t crc = 0xFFFFFFFFu;
    uint8_t buf[65536];
    size_t left = n;
    while (left) {
        size_t want = left < sizeof buf ? left : sizeof buf;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) { fclose(f); return 1; }
        for (size_t i = 0; i < got; ++i) {
            crc ^= buf[i];
            for (int b = 0; b < 8; ++b)
                crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
        left -= got;
    }
    fclose(f);
    *out = crc ^ 0xFFFFFFFFu;
    return 0;
}

#define CVM_SEAL_MAGIC_LE 0x314D5243u   /* 'C','R','M','1' */

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
                     const uint8_t *meta, uint32_t meta_size,
                     int seal,
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
                           + (rom_size           > 0 ? 1u : 0u)
                           + (meta_size          > 0 ? 1u : 0u)
                           + (seal               ? 1u : 0u);
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
    uint32_t meta_off    = meta_size > 0
                         ? code_off + code_size + data_size + imports_size
                                    + funcs_size + regions_size + rom_size : 0;
    /* The seal payload (12 B) goes last; its CRC covers everything before it. */
    uint32_t seal_off    = code_off + code_size + data_size + imports_size
                         + funcs_size + regions_size + rom_size + meta_size;

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
    if (meta_size > 0) {
        memset(sec, 0, sizeof(sec));
        put_u32_le(sec + 0,  CVM_SEC_META);
        put_u32_le(sec + 4,  meta_off);
        put_u32_le(sec + 8,  meta_size);
        put_u32_le(sec + 12, 0u);
        fwrite(sec, sizeof(sec), 1, f);
    }
    if (seal) {
        memset(sec, 0, sizeof(sec));
        put_u32_le(sec + 0,  CVM_SEC_SEAL);
        put_u32_le(sec + 4,  seal_off);
        put_u32_le(sec + 8,  12u);
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
    if (meta_size > 0)    fwrite(meta, 1, meta_size, f);
    if (seal) {                                    /* magic + version + crc(=0) */
        uint8_t s[12];
        put_u32_le(s + 0, CVM_SEAL_MAGIC_LE);
        put_u32_le(s + 4, 1u);
        put_u32_le(s + 8, 0u);                     /* patched below */
        fwrite(s, sizeof s, 1, f);
    }
    free(imports_buf);
    free(funcs_buf);
    free(regions_buf);

    int err = ferror(f);
    fclose(f);
    if (err) {
        fprintf(stderr, "translator: write failed for '%s'\n", path);
        return 1;
    }

    /* Compute the seal's CRC over [0, seal_off) and patch it into the payload. */
    if (seal) {
        uint32_t crc = 0;
        if (crc32_file_prefix(path, seal_off, &crc) != 0) {
            fprintf(stderr, "translator: seal CRC read failed for '%s'\n", path);
            return 1;
        }
        FILE *pf = fopen(path, "rb+");
        if (!pf) { fprintf(stderr, "translator: seal patch open failed\n"); return 1; }
        uint8_t c[4]; put_u32_le(c, crc);
        if (fseek(pf, (long)(seal_off + 8), SEEK_SET) != 0 ||
            fwrite(c, sizeof c, 1, pf) != 1) {
            fclose(pf); fprintf(stderr, "translator: seal patch write failed\n"); return 1;
        }
        fclose(pf);
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
            "  --meta=FILE          Append FILE as a host-only CVM_SEC_META blob\n"
            "  --seal               Append an integrity seal (magic + crc32)\n"
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
    g_cur_inst = NULL;   /* never carry a pointer into a previous iteration's IR */

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
        free(cg.val_slot);
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
/* Which soft runtimes the module needs (a CVM_PROBE_* bitmask): f64 if any
 * `double` appears (instruction result or operand), i64 if any i64 div/rem
 * appears (these lower to a runtime call; the other i64 ops are inline).
 * cvm-cc consults this (via --probe-runtime) to auto-link the matching TU
 * only when needed, so integer-only modules carry no soft code. */
static int module_runtime_needs(LLVMModuleRef mod) {
    int need = 0;
    for (LLVMValueRef fn = LLVMGetFirstFunction(mod);
         fn; fn = LLVMGetNextFunction(fn))
    {
        if (LLVMIsDeclaration(fn)) continue;
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn);
             bb; bb = LLVMGetNextBasicBlock(bb))
        {
            for (LLVMValueRef i = LLVMGetFirstInstruction(bb);
                 i; i = LLVMGetNextInstruction(i))
            {
                LLVMOpcode op = LLVMGetInstructionOpcode(i);
                if ((op == LLVMUDiv || op == LLVMSDiv ||
                     op == LLVMURem || op == LLVMSRem) &&
                    cg_type_is_i64(LLVMTypeOf(i)))
                    need |= CVM_PROBE_I64;
                if (cg_type_is_f64(LLVMTypeOf(i))) { need |= CVM_PROBE_F64; continue; }
                unsigned n = LLVMGetNumOperands(i);
                for (unsigned k = 0; k < n; ++k) {
                    LLVMValueRef o = LLVMGetOperand(i, k);
                    if (o && cg_type_is_f64(LLVMTypeOf(o))) { need |= CVM_PROBE_F64; break; }
                }
            }
        }
    }
    return need;
}

int main(int argc, char **argv) {
    const char *input  = NULL;
    const char *output = NULL;
    int         probe_runtime = 0;   /* --probe-runtime: print needed runtimes */
    uint32_t    heap_reserve = 0;
    uint32_t    stack_reserve = 0;
    int         stack_reserve_set = 0;
    const char *rom_path = NULL;
    const char *meta_path = NULL;
    int         seal_flag = 0;
    struct cli_region cli_regions[MAX_CLI_REGIONS];
    int         cli_region_count = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { usage(); return 2; }
            output = argv[++i];
        } else if (strcmp(argv[i], "--probe-runtime") == 0) {
            probe_runtime = 1;
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
        } else if (strncmp(argv[i], "--meta=", 7) == 0) {
            meta_path = argv[i] + 7;
        } else if (strcmp(argv[i], "--seal") == 0) {
            seal_flag = 1;
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

    /* Probe mode: report which runtime libraries the module needs, then exit
     * without translating. Used by cvm-cc to auto-link the soft-float runtime
     * only for modules that actually use `double`. The result is signalled via
     * the EXIT CODE (cvm-cc doesn't capture stdout): CVM_PROBE_F64 means "needs
     * the f64 soft-float runtime", 0 means "no runtime needed". A human-
     * readable token is also printed. (Bitcode/IO errors above already
     * returned 1, so the probe codes can't collide with those.) */
    if (probe_runtime) {
        int need = module_runtime_needs(mod);
        if (need & CVM_PROBE_F64) printf("f64\n");
        if (need & CVM_PROBE_I64) printf("i64\n");
        LLVMDisposeModule(mod);
        LLVMContextDispose(ctx);
        return need;
    }

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

        /* --meta=FILE: opaque host-only metadata blob, appended as CVM_SEC_META. */
        uint8_t *meta_buf = NULL;
        uint32_t meta_size = 0;
        if (rc == 0 && meta_path) {
            FILE *mf = fopen(meta_path, "rb");
            if (!mf) {
                fprintf(stderr, "translator: cannot read --meta file '%s': %s\n",
                        meta_path, strerror(errno));
                rc = 1;
            } else {
                fseek(mf, 0, SEEK_END);
                long n = ftell(mf);
                rewind(mf);
                if (n < 0 || (unsigned long)n > 0xFFFFFFFFu) {
                    fprintf(stderr, "translator: --meta file '%s' too large\n", meta_path);
                    rc = 1;
                } else if (n > 0) {
                    meta_buf = (uint8_t *)malloc((size_t)n);
                    if (!meta_buf || fread(meta_buf, 1, (size_t)n, mf) != (size_t)n) {
                        fprintf(stderr, "translator: failed reading --meta file '%s'\n", meta_path);
                        rc = 1;
                    } else {
                        meta_size = (uint32_t)n;
                    }
                }
                fclose(mf);
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
                          meta_buf, meta_size,
                          seal_flag,
                          entry_off) != 0)
            {
                rc = 1;
            } else {
                printf("translator: wrote %s (%u instructions, %u data bytes, "
                       "%d imports, %d funcs, %u heap-reserve, "
                       "%u stack-reserve, %d regions, %u rom-bytes, "
                       "%u meta-bytes)\n",
                       output, cg.count, globals.data_size,
                       cg.import_count, cg.func_count,
                       heap_reserve, stack_size, cli_region_count, rom_size,
                       meta_size);

                /* Optional symbol sidecar for the self-time profiler: with
                 * CVM_SYMS set, write "<output>.sym" mapping each FUNCS index
                 * to its entry instruction and source name. Index order here
                 * matches CALL imm24 / cvm_image.func_offsets exactly. */
                if (getenv("CVM_SYMS")) {
                    size_t olen = strlen(output);
                    char *sympath = (char *)malloc(olen + 5);
                    if (sympath) {
                        memcpy(sympath, output, olen);
                        memcpy(sympath + olen, ".sym", 5);
                        FILE *sf = fopen(sympath, "wb");
                        if (sf) {
                            /* User function k lives at FUNCS[k+1] (slot 0 is
                             * the reserved null-fn-ptr trap), so the runtime
                             * FUNCS index — what the interpreter and profiler
                             * see as the call target — is k+1. */
                            for (int k = 0; k < cg.func_count; ++k)
                                fprintf(sf, "%d\t%u\t%s\n", k + 1,
                                        cg.funcs[k].entry_offset,
                                        value_name(cg.funcs[k].value));
                            fclose(sf);
                            printf("translator: wrote %s (%d symbols)\n",
                                   sympath, cg.func_count);
                        } else {
                            fprintf(stderr, "translator: cannot write '%s'\n",
                                    sympath);
                        }
                        free(sympath);
                    }
                }
            }
        }
        free(rom_buf);
        free(meta_buf);
        free(cg.code);
        free(cg.vals);
        free(cg.regs);
        free(cg.val_slot);
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
