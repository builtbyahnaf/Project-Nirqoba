// Benchmark 01 - Managed-object allocation, Java track.
//
// One JVM object per node, linked into a chain. The static sink prevents the JIT
// from proving the chain does not escape and scalar-replacing the allocations.
// Allocation, traversal, and a forced collection are timed separately.
import java.util.Locale;

public final class Benchmark01Allocation {
    private static final int OBJECT_COUNT = 1_000_000;
    private static final int WARMUP_ROUNDS = 2;
    private static final int MEASURE_ROUNDS = 5;

    private static final class Node {
        Node next;
        long value;
    }

    // The sink is written during construction and cleared before collection.
    // Without it, escape analysis could remove the whole chain in a warmed JVM.
    private static Node sink;

    private static Node buildChain(int count) {
        Node head = new Node();
        head.value = 0;
        Node tail = head;
        for (int index = 1; index < count; ++index) {
            Node node = new Node();
            node.value = index;
            tail.next = node;
            tail = node;
        }
        return head;
    }

    private static long sumChain(Node head, int count) {
        long total = 0;
        Node cursor = head;
        for (int index = 0; index < count; ++index) {
            total += cursor.value;
            cursor = cursor.next;
        }
        return total;
    }

    private static long[] runRound() {
        long allocStart = System.nanoTime();
        Node head = buildChain(OBJECT_COUNT);
        sink = head;
        long allocEnd = System.nanoTime();

        long traverseStart = System.nanoTime();
        long checksum = sumChain(head, OBJECT_COUNT);
        long traverseEnd = System.nanoTime();

        return new long[] {
            allocEnd - allocStart,
            traverseEnd - traverseStart,
            checksum
        };
    }

    private static String ms(long nanos) {
        return String.format(Locale.ROOT, "%.3f", nanos / 1_000_000.0);
    }

    public static void main(String[] args) {
        for (int warmup = 0; warmup < WARMUP_ROUNDS; ++warmup) {
            runRound();
            sink = null;
            System.gc();
        }

        System.out.println("TEST managed_object_allocation");
        System.out.println("IMPL java");
        System.out.println("OBJECTS " + OBJECT_COUNT);
        System.out.println("WARMUP_ROUNDS " + WARMUP_ROUNDS);
        System.out.println("MEASURE_ROUNDS " + MEASURE_ROUNDS);

        for (int round = 0; round < MEASURE_ROUNDS; ++round) {
            System.out.println("ROUND " + round);
            long[] result = runRound();
            sink = null;

            long collectStart = System.nanoTime();
            System.gc();
            long collectEnd = System.nanoTime();

            System.out.println("ALLOC_MS " + ms(result[0]));
            System.out.println("TRAVERSE_MS " + ms(result[1]));
            System.out.println("COLLECT_MS " + ms(collectEnd - collectStart));
            System.out.println("CHECKSUM " + result[2]);
            // The JVM does not expose a live-object count. The checksum and the
            // WeakReference verification in test 03 are the correctness checks.
            System.out.println("LIVE_AFTER -1");
        }
    }
}
