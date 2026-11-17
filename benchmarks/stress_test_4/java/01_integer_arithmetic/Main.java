public class Main {
    public static void main(String[] args) {
        long totalOps = 500_000_000L;
        if (args.length > 0) {
            totalOps = Long.parseLong(args[0]);
        }

        long t0 = System.nanoTime();
        long total = 0L;

        for (long i = 1; i <= totalOps; i++) {
            total = (total + i);
            total = (total - (i & 0xFFL));
            total = (total * 3L);
            long div = (i % 7L) + 1L;
            total = (total ^ (total / div));
            total = (total ^ (total << 5) ^ (total >>> 59));
        }

        long t1 = System.nanoTime();
        double timeMs = (t1 - t0) / 1_000_000.0;

        System.out.println("WORKLOAD: ST4-01");
        System.out.println("LANGUAGE: Java");
        System.out.println("OPS: " + totalOps);
        System.out.printf("TIME_MS: %.3f\n", timeMs);
        System.out.println("CHECKSUM: " + total);
        System.out.println("STATUS: PASS");
    }
}
