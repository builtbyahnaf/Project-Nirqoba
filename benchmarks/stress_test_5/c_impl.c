/* Stress Test 5 - the CDMM gauntlet, manual baseline.
 *
 * C is the reference: every allocation here is an explicit malloc whose size is
 * the size the Nirqoba compiler chose for the same tier, so the comparison is
 * about allocator work and nothing else.
 *
 * The four phases mirror nirqoba_impl.nrqba one for one and print the same four
 * checksums.
 */

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int x;
} Vec1;

typedef struct {
    int kind;
    int payload;
} Event;

typedef struct CycleNode {
    int value;
    struct CycleNode *next;
} CycleNode;

#define STACK_ITERATIONS 200000
#define POOL_ITERATIONS 2000
#define POOL_SLOTS 8192
#define TREE_NODES 8191
#define GC_NODES 40000

static long long stack_phase(void) {
    long long checksum = 0;
    for (int i = 0; i < STACK_ITERATIONS; i++) {
        Vec1 *v = (Vec1 *)malloc(sizeof(Vec1));
        v->x = i;
        checksum += v->x;
        free(v);
    }
    return checksum;
}

static long long pool_phase(void) {
    long long events = 0;
    for (int i = 0; i < POOL_ITERATIONS; i++) {
        Event *batch = (Event *)malloc(sizeof(Event) * POOL_SLOTS);
        batch[0].kind = i;
        batch[0].payload = i * 2;
        events += batch[0].kind + batch[0].payload;
        free(batch);
    }
    return events;
}

static long long arena_phase(void) {
    int *tree = (int *)malloc(sizeof(int) * TREE_NODES * 3);
    tree[0] = 5;
    tree[1] = 1;
    tree[2] = 2;
    long long tree_sum = tree[0] + tree[1] + tree[2];

    Vec1 *leaf = (Vec1 *)malloc(sizeof(Vec1));
    leaf->x = 4;
    tree_sum += leaf->x;
    free(leaf);

    free(tree);
    return tree_sum;
}

static long long gc_phase(void) {
    CycleNode **kept = (CycleNode **)malloc(sizeof(CycleNode *) * GC_NODES);
    long long cycles = 0;
    for (int i = 0; i < GC_NODES; i++) {
        CycleNode *node = (CycleNode *)malloc(sizeof(CycleNode));
        node->value = i;
        node->next = NULL;
        kept[i] = node;
        cycles += node->value;
    }

    /* Roots dropped: an unreachable graph is reclaimed, one node at a time. */
    for (int i = 0; i < GC_NODES; i++) {
        free(kept[i]);
    }
    free(kept);
    return cycles;
}

int main(void) {
    printf("%lld\n", stack_phase());
    printf("%lld\n", pool_phase());
    printf("%lld\n", arena_phase());
    printf("%lld\n", gc_phase());
    return 0;
}
