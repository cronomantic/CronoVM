/* Pointer-relocation global initializers. Exercises serializing globals whose
 * initializers take the address of other globals: a table of {char*, int} with
 * the char* pointing at string literals, plus a global pointer to a data
 * global. Indexed at runtime (variable i) so nothing is constant-folded away.
 * vm_main(1,0) = tbl[1].v + tbl[1].name[0] + *pshared = 20 + 'B'(66) + 5 = 91. */
typedef struct { const char *name; int v; } ent_t;

static const ent_t tbl[] = { {"AAA", 10}, {"BBB", 20}, {"CCC", 30} };

static int shared = 5;
static int * const pshared = &shared;

int vm_main(int i, int j)
{
    (void)j;
    return tbl[i].v + (int)tbl[i].name[0] + *pshared;
}
