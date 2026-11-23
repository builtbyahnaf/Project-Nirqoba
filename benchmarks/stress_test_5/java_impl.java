// Stress Test 5 - the CDMM gauntlet, Java baseline.
//
// G1 is the collector. The short-lived values are one allocation each, the tree
// is a single int block, and the dropped graph is an ArrayList of nodes whose
// reference is cleared before System.gc().

import java.util.ArrayList;

public final class java_impl {

    static final class Vec1 {
        int x;
    }

    static final class Event {
        int kind;
        int payload;
    }

    static final class CycleNode {
        int value;
        CycleNode next;
    }

    static final int STACK_ITERATIONS = 200_000;
    static final int POOL_ITERATIONS = 2_000;
    static final int POOL_SLOTS = 8_192;
    static final int TREE_NODES = 8_191;
    static final int GC_NODES = 40_000;

    static long stackPhase() {
        long checksum = 0;
        for (int i = 0; i < STACK_ITERATIONS; i++) {
            Vec1 v = new Vec1();
            v.x = i;
            checksum += v.x;
        }
        return checksum;
    }

    static long poolPhase() {
        long events = 0;
        for (int i = 0; i < POOL_ITERATIONS; i++) {
            Event[] batch = new Event[POOL_SLOTS];
            batch[0] = new Event();
            batch[0].kind = i;
            batch[0].payload = i * 2;
            events += batch[0].kind + batch[0].payload;
        }
        return events;
    }

    static long arenaPhase() {
        int[] tree = new int[TREE_NODES * 3];
        tree[0] = 5;
        tree[1] = 1;
        tree[2] = 2;
        long treeSum = tree[0] + tree[1] + tree[2];

        Vec1 leaf = new Vec1();
        leaf.x = 4;
        treeSum += leaf.x;
        return treeSum;
    }

    static long gcPhase() {
        ArrayList<CycleNode> kept = new ArrayList<>(GC_NODES);
        long cycles = 0;
        for (int i = 0; i < GC_NODES; i++) {
            CycleNode node = new CycleNode();
            node.value = i;
            kept.add(node);
            cycles += node.value;
        }
        kept.clear();
        System.gc();
        return cycles;
    }

    public static void main(String[] args) {
        System.out.println(stackPhase());
        System.out.println(poolPhase());
        System.out.println(arenaPhase());
        System.out.println(gcPhase());
    }
}
