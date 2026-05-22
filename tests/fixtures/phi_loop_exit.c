/* Phi-move isolation across a conditional branch (the "lost-copy" problem).
 *
 * A single-block loop `while (*rover) rover = &(*rover)->next;` makes `rover` a
 * phi fed by its own back-edge with `&(*rover)->next`. The loop-EXIT edge then
 * reads `rover` (to store through it). If the translator emits the back-edge
 * phi move BEFORE the conditional branch (unconditionally), the exit edge sees
 * the back-edge value: when `*rover` is null, `&(*rover)->next` is `null + off`
 * (a tiny bogus address), so the store lands in garbage. This is exactly Doom's
 * GenerateTextureHashTable chain-append, which left the texture hash table
 * empty — every R_CheckTextureNumForName returned -1 ("could not add ...").
 *
 * phi_main(4): append pool[0..3] onto two chains (table[i&1]), then sum every
 * chained value. table[0] <- pool[0],pool[2] (v=10,12); table[1] <- pool[1],
 * pool[3] (v=11,13). sum = 10+12+11+13 = 46. With the bug the appends store
 * through a corrupted `rover`, the chains stay empty, and the sum is 0.
 */
typedef struct node { struct node *next; int v; } node_t;

static node_t *table[2];
static node_t  pool[8];

int phi_main(int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        node_t **rover = &table[i & 1];
        while (*rover != 0)
            rover = &(*rover)->next;     /* phi `rover`, read again on exit */
        pool[i].next = 0;
        pool[i].v = i + 10;
        *rover = &pool[i];               /* store through the exit-edge `rover` */
    }

    int sum = 0;
    for (int b = 0; b < 2; b++)
        for (node_t *p = table[b]; p != 0; p = p->next)
            sum += p->v;
    return sum;
}
