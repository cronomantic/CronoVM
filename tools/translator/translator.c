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

struct cg_fixup {
    uint32_t inst_index;
    LLVMBasicBlockRef target;
    int      shift;     /* bit position of the imm field */
    int      bits;      /* width of the imm field (8 or 24) */
};

struct cg {
    uint32_t *code;
    uint32_t  count;
    uint32_t  cap;

    /* SSA value -> physical register, parallel arrays. */
    LLVMValueRef *vals;
    uint8_t      *regs;
    int           map_count;
    int           map_cap;
    int           next_reg;

    /* Block enumeration and starting offsets in code[]. */
    LLVMBasicBlockRef *blocks;
    uint32_t          *block_offsets;
    int                block_count;
    int                block_cap;

    /* Pending branch fixups, resolved after all blocks emitted. */
    struct cg_fixup *fixups;
    int              fixup_count;
    int              fixup_cap;

    uint8_t           zero_reg;       /* register holding 0, for branch tests */
    LLVMBasicBlockRef cur_block;      /* updated during emit walk */

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
        if (imm < INT16_MIN || imm > INT16_MAX) {
            ERR(cg->fn_name,
                "constant %lld doesn't fit in MOVI imm16 "
                "(wide-constant lowering not yet implemented)", imm);
            cg->had_error = 1;
            return 0;
        }
        uint8_t r = cg_alloc_reg(cg);
        cg_emit(cg, enc_i16(CVM_OP_MOVI, r, (int16_t)imm));
        return r;
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
            ERR(cg->fn_name,
                "branch offset %d out of range [%d..%d] for %d-bit field "
                "(trampolining not yet implemented)",
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

static void cg_pre_alloc_function(struct cg *cg, LLVMValueRef fn) {
    unsigned np = LLVMCountParams(fn);
    for (unsigned i = 0; i < np; ++i) {
        LLVMValueRef p = LLVMGetParam(fn, i);
        cg_assign(cg, p, cg_alloc_reg(cg));   /* R0..R(N-1) */
    }
    cg->zero_reg = cg_alloc_reg(cg);          /* dedicated zero register */

    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn);
         bb; bb = LLVMGetNextBasicBlock(bb))
    {
        cg_register_block(cg, bb);
        for (LLVMValueRef i = LLVMGetFirstInstruction(bb);
             i; i = LLVMGetNextInstruction(i))
        {
            LLVMTypeRef ty = LLVMTypeOf(i);
            if (LLVMGetTypeKind(ty) == LLVMVoidTypeKind) continue;
            cg_assign(cg, i, cg_alloc_reg(cg));
        }
    }
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

    cg_pre_alloc_function(cg, fn);
    if (cg->had_error) return 1;

    /* Prologue: materialise the zero register once. */
    cg_emit(cg, enc_i16(CVM_OP_MOVI, cg->zero_reg, 0));

    for (int b = 0; b < cg->block_count && !cg->had_error; ++b) {
        LLVMBasicBlockRef bb = cg->blocks[b];
        cg->block_offsets[b] = cg->count;
        cg->cur_block = bb;

        for (LLVMValueRef i = LLVMGetFirstInstruction(bb);
             i && !cg->had_error; i = LLVMGetNextInstruction(i))
        {
            LLVMOpcode op = LLVMGetInstructionOpcode(i);
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

            case LLVMRet: {
                if (LLVMGetNumOperands(i) == 0) {
                    cg_emit(cg, enc_r(CVM_OP_HALT, 0, 0, 0));
                } else {
                    uint8_t r = cg_reg_for(cg, LLVMGetOperand(i, 0));
                    cg_emit(cg, enc_r(CVM_OP_HALT, r, 0, 0));
                }
                break;
            }

            case LLVMCall: {
                LLVMValueRef callee = LLVMGetCalledValue(i);
                size_t name_len = 0;
                const char *name = callee
                    ? LLVMGetValueName2(callee, &name_len) : NULL;
                if (!name || name_len == 0) {
                    ERR(cg->fn_name, "call with unknown callee");
                    cg->had_error = 1;
                    break;
                }

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

                if (strncmp(name, "llvm.", 5) == 0) {
                    ERR(cg->fn_name,
                        "intrinsic '%s' not yet lowered", name);
                } else {
                    ERR(cg->fn_name,
                        "user-defined call to '%s' not yet lowered "
                        "(CALL/RET pending)", name);
                }
                cg->had_error = 1;
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

    if (!cg->had_error && cg_resolve_fixups(cg) != 0)
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
        free(cg.blocks);
        free(cg.block_offsets);
        free(cg.fixups);
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
