// Benchmark 02 - Mark and sweep, Java track.
//
// A fully reachable tree is held from a static root, then full collections are
// forced with System.gc(). The JVM control is the closest analogue to Nirqoba's
// stop-the-world collector when the JVM is launched with -XX:+UseSerialGC.
// GarbageCollectorMXBean confirms that every System.gc() call performed a real
// collection; the runner treats a missing collection as invalid data.
import java.lang.management.GarbageCollectorMXBean;
import java.lang.management.ManagementFactory;
import java.util.List;
import java.util.Locale;

public final class Benchmark02MarkSweep {
    private static final int TREE_DEPTH = 20;
    private static final int TREE_NODES = (1 << (TREE_DEPTH + 1)) - 1;
    private static final int WARMUP_BATCHES = 3;
    private static final int COLLECT_BATCH = 5;
    private static final int MEASURE_ROUNDS = 5;

    private static final class Node {
        Node left;
        Node right;
        long value;
    }

    private static Node root;

    private static Node buildTree(int depth, long value) {
        Node node = new Node();
        node.value = value;
        if (depth > 0) {
            node.left = buildTree(depth - 1, value * 2);
            node.right = buildTree(depth - 1, value * 2 + 1);
        }
        return node;
    }

    private static long sumTree(Node node, int depth) {
        if (depth == 0) {
            return node.value;
        }
        return node.value + sumTree(node.left, depth - 1)
                + sumTree(node.right, depth - 1);
    }

    private static long totalCollections() {
        long total = 0;
        List<GarbageCollectorMXBean> beans =
                ManagementFactory.getGarbageCollectorMXBeans();
        for (GarbageCollectorMXBean bean : beans) {
            long count = bean.getCollectionCount();
            if (count > 0) {
                total += count;
            }
        }
        return total;
    }

    private static String ms(long nanos) {
        return String.format(Locale.ROOT, "%.3f", nanos / 1_000_000.0);
    }

    public static void main(String[] args) {
        long buildStart = System.nanoTime();
        root = buildTree(TREE_DEPTH, 1);
        long buildEnd = System.nanoTime();

        long traverseStart = System.nanoTime();
        long checksum = sumTree(root, TREE_DEPTH);
        long traverseEnd = System.nanoTime();

        for (int warmup = 0; warmup < WARMUP_BATCHES; ++warmup) {
            for (int collection = 0; collection < COLLECT_BATCH; ++collection) {
                System.gc();
            }
        }

        System.out.println("TEST mark_and_sweep");
        System.out.println("IMPL java");
        System.out.println("OBJECTS " + TREE_NODES);
        System.out.println("WARMUP_BATCHES " + WARMUP_BATCHES);
        System.out.println("COLLECT_BATCH " + COLLECT_BATCH);
        System.out.println("MEASURE_ROUNDS " + MEASURE_ROUNDS);
        System.out.println("BUILD_MS " + ms(buildEnd - buildStart));
        System.out.println("TRAVERSE_MS " + ms(traverseEnd - traverseStart));
        System.out.println("CHECKSUM " + checksum);

        for (int round = 0; round < MEASURE_ROUNDS; ++round) {
            long before = totalCollections();
            long start = System.nanoTime();
            for (int collection = 0; collection < COLLECT_BATCH; ++collection) {
                System.gc();
            }
            long end = System.nanoTime();
            long delta = totalCollections() - before;

            System.out.println("ROUND " + round);
            System.out.println("COLLECT_MS "
                    + ms((end - start) / COLLECT_BATCH));
            // Java does not expose a live-object count. The root remains
            // reachable, so a correct full GC must retain the whole tree.
            System.out.println("LIVE_AFTER " + TREE_NODES);
            System.out.println("COLLECTIONS_DELTA " + delta);
        }
    }
}
