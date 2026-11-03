// Benchmark 03 - Cyclic graph reclamation, Java track.
//
// Each pair of nodes points only at its partner. A static array of
// WeakReferences lets the benchmark prove that the JVM actually reclaimed the
// cycles rather than merely moving them. Collection is forced with System.gc().
import java.lang.ref.WeakReference;
import java.util.Locale;

public final class Benchmark03Cycles {
    private static final int CYCLE_PAIRS = 500_000;
    private static final int CYCLE_NODES = CYCLE_PAIRS * 2;
    private static final int WARMUP_ROUNDS = 2;
    private static final int MEASURE_ROUNDS = 5;

    private static final class Node {
        Node next;
        long value;
    }

    // WeakReferences do not keep the cycles alive; they only let the benchmark
    // observe whether each pair was reclaimed.
    private static WeakReference<Node>[] references;

    @SuppressWarnings("unchecked")
    private static long buildCycles(int pairs) {
        references = (WeakReference<Node>[]) new WeakReference<?>[pairs];
        long checksum = 0;
        for (int index = 0; index < pairs; ++index) {
            Node first = new Node();
            Node second = new Node();
            first.value = index * 2L;
            second.value = index * 2L + 1L;
            first.next = second;
            second.next = first;
            references[index] = new WeakReference<>(first);
            checksum += first.value + second.value;
        }
        return checksum;
    }

    private static int countCleared() {
        int cleared = 0;
        for (WeakReference<Node> reference : references) {
            if (reference.get() == null) {
                ++cleared;
            }
        }
        return cleared;
    }

    private static String ms(long nanos) {
        return String.format(Locale.ROOT, "%.3f", nanos / 1_000_000.0);
    }

    public static void main(String[] args) {
        for (int warmup = 0; warmup < WARMUP_ROUNDS; ++warmup) {
            buildCycles(CYCLE_PAIRS);
            System.gc();
        }

        System.out.println("TEST cyclic_graph_reclamation");
        System.out.println("IMPL java");
        System.out.println("OBJECTS " + CYCLE_NODES);
        System.out.println("WARMUP_ROUNDS " + WARMUP_ROUNDS);
        System.out.println("MEASURE_ROUNDS " + MEASURE_ROUNDS);

        for (int round = 0; round < MEASURE_ROUNDS; ++round) {
            long buildStart = System.nanoTime();
            long checksum = buildCycles(CYCLE_PAIRS);
            long buildEnd = System.nanoTime();

            long collectStart = System.nanoTime();
            System.gc();
            long collectEnd = System.nanoTime();
            int cleared = countCleared();

            System.out.println("ROUND " + round);
            System.out.println("BUILD_MS " + ms(buildEnd - buildStart));
            System.out.println("COLLECT_MS " + ms(collectEnd - collectStart));
            System.out.println("CHECKSUM " + checksum);
            System.out.println("LIVE_AFTER " + (CYCLE_PAIRS - cleared));
            System.out.println("CYCLES_CLEARED " + cleared);
        }
    }
}
