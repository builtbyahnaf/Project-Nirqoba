public class Main {
    public static void main(String[] args) {
        int n = 50_000_000;
        if (args.length > 0) {
            n = Integer.parseInt(args[0]);
        }

        long t0 = System.nanoTime();
        long[] vec = new long[n];
        for (int i = 0; i < n; i++) {
            vec[i] = ((long)i) ^ 0x55555555L;
        }
        long t1 = System.nanoTime();
        double constructMs = (t1 - t0) / 1_000_000.0;

        long t2 = System.nanoTime();
        long total = 0L;
        for (int i = 0; i < n; i++) {
            total += vec[i];
        }
        long t3 = System.nanoTime();
        double reduceMs = (t3 - t2) / 1_000_000.0;

        System.out.println("WORKLOAD: ST4-05");
        System.out.println("LANGUAGE: Java");
        System.out.println("ELEMENTS: " + n);
        System.out.printf("CONSTRUCTION_MS: %.3f\n", constructMs);
        System.out.printf("REDUCTION_MS: %.3f\n", reduceMs);
        System.out.printf("TIME_MS: %.3f\n", constructMs + reduceMs);
        System.out.println("CHECKSUM: " + total);
        System.out.println("STATUS: PASS");
    }
}
