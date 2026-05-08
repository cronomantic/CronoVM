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
#include <llvm-c/Types.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    case LLVMHalfTypeKind:
    case LLVMBFloatTypeKind:
    case LLVMFloatTypeKind:
    case LLVMDoubleTypeKind:
    case LLVMX86_FP80TypeKind:
    case LLVMFP128TypeKind:
    case LLVMPPC_FP128TypeKind:
        snprintf(err, errlen,
                 "floating-point types not yet supported "
                 "(deferred to float64 opcodes)");
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

/* --- codegen ------------------------------------------------------------- */

struct cg {
    uint32_t *code;
    uint32_t  count;
    uint32_t  cap;

    /* SSA value -> physical register, parallel arrays. Linear scan is fine
     * for the function sizes we currently target. */
    LLVMValueRef *vals;
    uint8_t      *regs;
    int           map_count;
    int           map_cap;
    int           next_reg;

    int           had_error;
    const char   *fn_name;
};

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

static void cg_emit(struct cg *cg, uint32_t inst) {
    if (cg->count == cg->cap) {
        cg->cap = cg->cap ? cg->cap * 2 : 64;
        cg->code = (uint32_t *)realloc(cg->code, cg->cap * sizeof(uint32_t));
        if (!cg->code) { perror("realloc"); exit(1); }
    }
    cg->code[cg->count++] = inst;
}

static uint8_t cg_alloc_reg(struct cg *cg) {
    if (cg->next_reg >= 256) {
        ERR(cg->fn_name, "ran out of registers (>256 live SSA values)");
        cg->had_error = 1;
        return 0;
    }
    return (uint8_t)cg->next_reg++;
}

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

/* Returns the physical register currently holding `v`. If `v` is a constant
 * not yet materialised, emits a MOVI for it and remembers the binding. */
static uint8_t cg_reg_for(struct cg *cg, LLVMValueRef v) {
    int idx = cg_lookup(cg, v);
    if (idx >= 0) return cg->regs[idx];

    if (LLVMIsAConstantInt(v)) {
        long long imm = LLVMConstIntGetSExtValue(v);
        if (imm < INT16_MIN || imm > INT16_MAX) {
            ERR(cg->fn_name,
                "constant %lld doesn't fit in MOVI imm16 "
                "(wide-constant lowering not yet implemented)", imm);
            cg->had_error = 1;
            return 0;
        }
        uint8_t r = cg_alloc_reg(cg);
        cg_emit(cg, enc_i16(CVM_OP_MOVI, r, (int16_t)imm));
        return cg_assign(cg, v, r);
    }

    ERR(cg->fn_name,
        "operand has no register assigned (use-before-def or unsupported value kind)");
    cg->had_error = 1;
    return 0;
}

static int cg_function(struct cg *cg, LLVMValueRef fn) {
    cg->fn_name = value_name(fn);

    unsigned n_params = LLVMCountParams(fn);
    if (n_params > 8) {
        ERR(cg->fn_name, "more than 8 parameters not supported (got %u)",
            n_params);
        cg->had_error = 1;
        return 1;
    }
    for (unsigned i = 0; i < n_params; ++i) {
        LLVMValueRef p = LLVMGetParam(fn, i);
        cg_assign(cg, p, cg_alloc_reg(cg));   /* R0, R1, ... */
    }

    unsigned bb_count = LLVMCountBasicBlocks(fn);
    if (bb_count != 1) {
        ERR(cg->fn_name,
            "multi-block control flow not yet supported (got %u blocks)",
            bb_count);
        cg->had_error = 1;
        return 1;
    }

    LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn);
    for (LLVMValueRef i = LLVMGetFirstInstruction(bb);
         i; i = LLVMGetNextInstruction(i))
    {
        LLVMOpcode op = LLVMGetInstructionOpcode(i);
        switch (op) {
        case LLVMAdd:
        case LLVMSub:
        case LLVMMul: {
            uint8_t lhs = cg_reg_for(cg, LLVMGetOperand(i, 0));
            uint8_t rhs = cg_reg_for(cg, LLVMGetOperand(i, 1));
            uint8_t dst = cg_alloc_reg(cg);
            cg_assign(cg, i, dst);
            uint8_t cv = (op == LLVMAdd) ? CVM_OP_ADD
                       : (op == LLVMSub) ? CVM_OP_SUB
                                         : CVM_OP_MUL;
            cg_emit(cg, enc_r(cv, dst, lhs, rhs));
            break;
        }
        case LLVMRet: {
            if (LLVMGetNumOperands(i) == 0) {
                cg_emit(cg, enc_r(CVM_OP_HALT, 0, 0, 0));
            } else {
                uint8_t r = cg_reg_for(cg, LLVMGetOperand(i, 0));
                cg_emit(cg, enc_r(CVM_OP_HALT, r, 0, 0));
            }
            break;
        }
        default:
            ERR(cg->fn_name,
                "%s: codegen not implemented yet", opcode_name(op));
            cg->had_error = 1;
            return 1;
        }
    }
    return cg->had_error ? 1 : 0;
}

/* --- binary writer ------------------------------------------------------- */

static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int write_bin(const char *path,
                     const uint32_t *code, uint32_t code_count,
                     uint32_t entry)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "translator: cannot open '%s' for writing: %s\n",
                path, strerror(errno));
        return 1;
    }

    uint32_t section_count = 1;
    uint32_t table_off = 24;
    uint32_t code_off  = table_off + section_count * 16;
    uint32_t code_size = code_count * 4u;

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

    for (uint32_t i = 0; i < code_count; ++i) {
        uint8_t b[4];
        put_u32_le(b, code[i]);
        fwrite(b, sizeof(b), 1, f);
    }

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
            "Usage: cvm-translate [-o <out.bin>] <input.bc>\n"
            "  -o <file>   Emit a CronoVM .bin (otherwise validate-only).\n");
}

int main(int argc, char **argv) {
    const char *input  = NULL;
    const char *output = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { usage(); return 2; }
            output = argv[++i];
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

    LLVMValueRef first_def = NULL;
    int defs = 0;
    for (LLVMValueRef fn = LLVMGetFirstFunction(mod);
         fn; fn = LLVMGetNextFunction(fn))
    {
        print_function_summary(fn);
        if (LLVMIsDeclaration(fn)) continue;
        if (!first_def) first_def = fn;
        ++defs;
        validate_function(fn);
    }
    if (defs == 0) ERR(NULL, "module contains no function definitions");

    int rc = 0;

    if (g_errors == 0 && output) {
        if (defs > 1) {
            ERR(NULL, "module has %d function definitions; "
                      "step-5 codegen handles exactly one", defs);
        }
    }

    if (g_errors == 0 && output) {
        struct cg cg = {0};
        if (cg_function(&cg, first_def) != 0) {
            rc = 1;
        } else if (write_bin(output, cg.code, cg.count, 0) != 0) {
            rc = 1;
        } else {
            printf("translator: wrote %s (%u instructions)\n",
                   output, cg.count);
        }
        free(cg.code);
        free(cg.vals);
        free(cg.regs);
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
