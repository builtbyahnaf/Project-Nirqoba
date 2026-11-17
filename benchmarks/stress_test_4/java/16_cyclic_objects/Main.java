public class Main {
    static class CycleNode {
        int value;
        CycleNode peer;
        CycleNode(int val) {
            this.value = val;
        }
    }

    public static void main(String[] args) {
        int pairsCount = 500_000;
        if (args.length > 0) {
            pairsCount = Integer.parseInt(args[0]);
        }

        long t0 = System.nanoTime();
        CycleNode[] aNodes = new CycleNode[pairsCount];
        CycleNode[] bNodes = new CycleNode[pairsCount];

        for (int i = 0; i < pairsCount; i++) {
            CycleNode a = new CycleNode(i);
            CycleNode b = new CycleNode(i);
            a.peer = b;
            b.peer = a;
            aNodes[i] = a;
            bNodes[i] = b;
        }
        long t1 = System.nanoTime();
        double buildMs = (t1 - t0) / 1_000_000.0;

        // Sever external roots
        aNodes = null;
        bNodes = null;

        long t2 = System.nanoTime();
        System.gc();
        long t3 = System.nanoTime();
        double reclaimMs = (t3 - t2) / 1_000_000.0;

        double totalMs = buildMs + reclaimMs;
        System.out.println("WORKLOAD: ST4-16");
        System.out.println("LANGUAGE: Java");
        System.out.println("CYCLES: " + pairsCount);
        System.out.printf("BUILD_MS: %.3f\n", buildMs);
        System.out.printf("RECLAIM_MS: %.3f\n", reclaimMs);
        System.out.printf("TIME_MS: %.3f\n", totalMs);
        System.out.println("STATUS: PASS");
    }
}
