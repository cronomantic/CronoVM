/* cvm-dis — a disassembler for CronoVM .crom/.bin images.
 *
 * Decodes the CODE section into human-readable instructions, labelling function
 * entry points and CALL targets via the optional `<image>.sym` sidecar (written
 * by cvm-translate when CVM_SYMS is set). FUNCS slots are (symidx)<<1 — the .sym
 * index column is symidx, so a slot maps to .sym index slot/2.
 *
 * Usage:
 *   cvm-dis <image.crom> [sym]                 # list functions (from .sym/FUNCS)
 *   cvm-dis <image.crom> [sym] --func <idx>    # disassemble symbol index <idx>
 *   cvm-dis <image.crom> [sym] --pc <pc> <n>   # disassemble n insts from pc
 *   cvm-dis <image.crom> [sym] --around <pc> <b> <a>  # b before, a after pc
 *
 * Build (standalone):
 *   clang -O2 -std=gnu23 -I <CronoVM>/include cvm-dis.c -o cvm-dis
 */
#include "cvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- symbol table (.sym): "<symidx>\t<entry_off>\t<name>" per line ---------- */
struct sym { uint32_t idx; uint32_t off; char name[256]; };
static struct sym *g_syms = NULL;
static int          g_nsyms = 0;

static void load_syms(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cvm-dis: no .sym (%s) — names unavailable\n", path); return; }
    int cap = 1024; g_syms = malloc((size_t)cap * sizeof *g_syms);
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (g_nsyms == cap) { cap *= 2; g_syms = realloc(g_syms, (size_t)cap * sizeof *g_syms); }
        struct sym *s = &g_syms[g_nsyms];
        char *t1 = strchr(line, '\t'); if (!t1) continue;
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue;
        *t1 = *t2 = 0;
        s->idx = (uint32_t)strtoul(line, NULL, 10);
        s->off = (uint32_t)strtoul(t1 + 1, NULL, 10);
        char *nl = strchr(t2 + 1, '\n'); if (nl) *nl = 0;
        snprintf(s->name, sizeof s->name, "%s", t2 + 1);
        ++g_nsyms;
    }
    fclose(f);
}
/* name for a FUNCS slot (= symidx<<1) */
static const char *slot_name(uint32_t slot) {
    uint32_t idx = slot >> 1;
    for (int i = 0; i < g_nsyms; ++i) if (g_syms[i].idx == idx) return g_syms[i].name;
    return "?";
}
/* enclosing symbol (entry off <= pc, maximal) */
static const struct sym *encl_sym(uint32_t pc) {
    const struct sym *best = NULL;
    for (int i = 0; i < g_nsyms; ++i)
        if (g_syms[i].off <= pc && (!best || g_syms[i].off > best->off)) best = &g_syms[i];
    return best;
}

static const char *opname(uint8_t op) {
    switch (op) {
    case CVM_OP_HALT: return "HALT"; case CVM_OP_MOVI: return "MOVI"; case CVM_OP_MOV: return "MOV";
    case CVM_OP_ADD: return "ADD"; case CVM_OP_SUB: return "SUB"; case CVM_OP_MUL: return "MUL";
    case CVM_OP_LDW: return "LDW"; case CVM_OP_STW: return "STW"; case CVM_OP_JMP: return "JMP";
    case CVM_OP_BEQ: return "BEQ"; case CVM_OP_BNE: return "BNE"; case CVM_OP_SYSCALL: return "SYSCALL";
    case CVM_OP_CMP_EQ: return "CMP_EQ"; case CVM_OP_CMP_NE: return "CMP_NE"; case CVM_OP_CMP_LT: return "CMP_LT";
    case CVM_OP_CMP_LE: return "CMP_LE"; case CVM_OP_CMP_LTU: return "CMP_LTU"; case CVM_OP_CMP_LEU: return "CMP_LEU";
    case CVM_OP_DIV: return "DIV"; case CVM_OP_DIVU: return "DIVU"; case CVM_OP_MOD: return "MOD"; case CVM_OP_MODU: return "MODU";
    case CVM_OP_SHL: return "SHL"; case CVM_OP_SHR: return "SHR"; case CVM_OP_SAR: return "SAR";
    case CVM_OP_AND: return "AND"; case CVM_OP_OR: return "OR"; case CVM_OP_XOR: return "XOR";
    case CVM_OP_CALL: return "CALL"; case CVM_OP_RET: return "RET"; case CVM_OP_CALLR: return "CALLR";
    case CVM_OP_LDB: return "LDB"; case CVM_OP_STB: return "STB"; case CVM_OP_LDH: return "LDH"; case CVM_OP_STH: return "STH";
    case CVM_OP_MOVHI: return "MOVHI"; case CVM_OP_MEMCPY: return "MEMCPY"; case CVM_OP_MEMSET: return "MEMSET"; case CVM_OP_MEMMOVE: return "MEMMOVE";
    case CVM_OP_MULH: return "MULH"; case CVM_OP_MULHU: return "MULHU";
    case CVM_OP_FADD: return "FADD"; case CVM_OP_FSUB: return "FSUB"; case CVM_OP_FMUL: return "FMUL"; case CVM_OP_FDIV: return "FDIV";
    case CVM_OP_FNEG: return "FNEG"; case CVM_OP_FCMP_EQ: return "FCMP_EQ"; case CVM_OP_FCMP_NE: return "FCMP_NE";
    case CVM_OP_FCMP_LT: return "FCMP_LT"; case CVM_OP_FCMP_LE: return "FCMP_LE";
    case CVM_OP_F2I_S: return "F2I_S"; case CVM_OP_F2I_U: return "F2I_U"; case CVM_OP_I2F_S: return "I2F_S"; case CVM_OP_I2F_U: return "I2F_U";
    case CVM_OP_JMPR: return "JMPR"; case CVM_OP_FSQRT: return "FSQRT"; case CVM_OP_QDIV1616: return "QDIV1616"; case CVM_OP_QDIV6432: return "QDIV6432";
    case CVM_OP_SETJMP: return "SETJMP"; case CVM_OP_LONGJMP: return "LONGJMP"; case CVM_OP_CORO_SWAP: return "CORO_SWAP";
    case CVM_OP_FFLOOR: return "FFLOOR"; case CVM_OP_FCEIL: return "FCEIL"; case CVM_OP_FTRUNC: return "FTRUNC";
    default: return "???";
    }
}

static void dis_one(uint32_t pc, uint32_t inst) {
    uint8_t op = (uint8_t)(inst & 0xFF);
    uint8_t a = (uint8_t)((inst >> 8) & 0xFF), b = (uint8_t)((inst >> 16) & 0xFF), c = (uint8_t)((inst >> 24) & 0xFF);
    int16_t imm16 = (int16_t)(uint16_t)((inst >> 16) & 0xFFFF);
    uint32_t imm24 = (inst >> 8) & 0xFFFFFFu;
    int32_t  soff24 = (int32_t)imm24; if (soff24 & 0x800000) soff24 -= 0x1000000;
    int8_t   boff = (int8_t)c;
    printf("%8u: %08x  %-8s", pc, inst, opname(op));
    switch (op) {
    case CVM_OP_MOVI:  printf("R%u, #%d (0x%x)", a, imm16, (uint16_t)imm16); break;
    case CVM_OP_MOVHI: printf("R%u, #0x%04x<<16", a, (uint16_t)imm16); break;
    case CVM_OP_MOV:   printf("R%u, R%u", a, b); break;
    case CVM_OP_LDW: case CVM_OP_LDB: case CVM_OP_LDH: printf("R%u, [R%u]", a, b); break;
    case CVM_OP_STW: case CVM_OP_STB: case CVM_OP_STH: printf("[R%u], R%u", b, c); break;
    case CVM_OP_MEMCPY: case CVM_OP_MEMMOVE: printf("[R%u], [R%u], R%u (dst,src,len)", a, b, c); break;
    case CVM_OP_MEMSET: printf("[R%u], R%u, R%u (dst,byte,len)", a, b, c); break;
    case CVM_OP_CALL:  printf("fn slot=%u  ; %s", imm24, slot_name(imm24)); break;
    case CVM_OP_CALLR: printf("R%u  ; (indirect)", a); break;
    case CVM_OP_RET: case CVM_OP_HALT: break;
    case CVM_OP_JMP:   printf("%+d -> %u", soff24, (uint32_t)((int32_t)pc + soff24)); break;
    case CVM_OP_JMPR:  printf("R%u", a); break;
    case CVM_OP_BEQ: case CVM_OP_BNE: printf("R%u, R%u, %+d -> %u", a, b, boff, (uint32_t)((int32_t)pc + boff)); break;
    case CVM_OP_SETJMP:  printf("R%u, [R%u]  (dest, env)", a, b); break;
    case CVM_OP_LONGJMP: printf("[R%u]", a); break;
    case CVM_OP_SYSCALL: printf("#%u", imm24); break;
    default:           printf("R%u, R%u, R%u", a, b, c); break;
    }
    putchar('\n');
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: cvm-dis <image> [sym] [--func N | --pc PC N | --around PC B A]\n"); return 2; }
    const char *imgpath = argv[1];
    /* arg2 is the sym path unless it starts with -- */
    int ai = 2;
    char symbuf[1024];
    if (argc >= 3 && strncmp(argv[2], "--", 2) != 0) { load_syms(argv[2]); ai = 3; }
    else { snprintf(symbuf, sizeof symbuf, "%s.sym", imgpath); load_syms(symbuf); ai = 2; }

    FILE *f = fopen(imgpath, "rb"); if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)len); if (fread(blob, 1, (size_t)len, f) != (size_t)len) { return 1; } fclose(f);

    if (memcmp(blob, "CVM1", 4) != 0) { fprintf(stderr, "bad magic\n"); return 1; }
    uint32_t sec_count = rd_u32(blob + 12), sec_tab = rd_u32(blob + 16), entry = rd_u32(blob + 20);
    uint32_t code_off = 0, code_size = 0;
    for (uint32_t i = 0; i < sec_count; ++i) {
        const uint8_t *s = blob + sec_tab + (size_t)i * 16u;
        uint32_t type = rd_u32(s), foff = rd_u32(s + 4), size = rd_u32(s + 8);
        if (type == CVM_SEC_CODE) { code_off = foff; code_size = size; }
    }
    if (!code_size) { fprintf(stderr, "no CODE section\n"); return 1; }
    const uint32_t *code = (const uint32_t *)(blob + code_off);
    uint32_t code_count = code_size / 4u;
    fprintf(stderr, "cvm-dis: %u instructions, entry=%u, %d symbols\n", code_count, entry, g_nsyms);

    if (ai >= argc) {   /* list functions */
        for (int i = 0; i < g_nsyms; ++i) printf("idx %-5u slot %-6u off %-9u %s\n",
                                                  g_syms[i].idx, g_syms[i].idx << 1, g_syms[i].off, g_syms[i].name);
        return 0;
    }
    uint32_t start = 0, count = 64;
    if (strcmp(argv[ai], "--func") == 0 && ai + 1 < argc) {
        uint32_t idx = (uint32_t)strtoul(argv[ai + 1], NULL, 0);
        for (int i = 0; i < g_nsyms; ++i) if (g_syms[i].idx == idx) { start = g_syms[i].off; break; }
        count = 200;
    } else if (strcmp(argv[ai], "--pc") == 0 && ai + 2 < argc) {
        start = (uint32_t)strtoul(argv[ai + 1], NULL, 0); count = (uint32_t)strtoul(argv[ai + 2], NULL, 0);
    } else if (strcmp(argv[ai], "--around") == 0 && ai + 3 < argc) {
        uint32_t pc = (uint32_t)strtoul(argv[ai + 1], NULL, 0);
        uint32_t before = (uint32_t)strtoul(argv[ai + 2], NULL, 0), after = (uint32_t)strtoul(argv[ai + 3], NULL, 0);
        start = pc > before ? pc - before : 0; count = before + after + 1;
    } else { fprintf(stderr, "bad args\n"); return 2; }

    const struct sym *e = encl_sym(start);
    if (e) printf("; in %s (idx %u, slot %u, entry off %u)\n", e->name, e->idx, e->idx << 1, e->off);
    for (uint32_t k = 0; k < count && start + k < code_count; ++k)
        dis_one(start + k, code[start + k]);
    return 0;
}
