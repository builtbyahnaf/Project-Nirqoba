public class Main {
    public static void main(String[] args) {
        long totalOps = 100_000_000L;
        if (args.length > 0) {
            totalOps = Long.parseLong(args[0]);
        }

        long t0 = System.nanoTime();
        double acc = 1.0;

        for (long i = 1; i <= totalOps; i++) {
            acc = acc + 0.001;
            acc = acc * 1.000001;
            acc = acc / 1.0000005;
            acc = acc + Math.sqrt(acc) * 0.00001;
        }

        long t1 = System.nanoTime();
        double timeMs = (t1 - t0) / 1_000_000.0;

        System.out.println("WORKLOAD: ST4-02");
        System.out.println("LANGUAGE: Java");
        System.out.println("OPS: " + totalOps);
        System.out.printf("TIME_MS: %.3f\n", timeMs);
        System.out.printf("CHECKSUM: %.6f\n", acc);
        System.out.println("STATUS: PASS");
    }
}
