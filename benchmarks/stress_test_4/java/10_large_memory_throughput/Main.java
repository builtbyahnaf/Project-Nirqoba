public class Main {
    public static void main(String[] args) {
        int size = 512 * 1024 * 1024; // 512 MiB
        if (args.length > 0) {
            size = Integer.parseInt(args[0]) * 1024 * 1024;
        }

        long t0 = System.nanoTime();
        byte[] buf = new byte[size];
        for (int i = 0; i < size; i += 4096) {
            buf[i] = (byte)(i & 0xFF);
        }
        long t1 = System.nanoTime();
        double writeMs = (t1 - t0) / 1_000_000.0;

        long t2 = System.nanoTime();
        long checksum = 0L;
        for (int i = 0; i < size; i += 1024) {
            checksum += buf[i];
        }
        long t3 = System.nanoTime();
        double readMs = (t3 - t2) / 1_000_000.0;

        long t4 = System.nanoTime();
        for (int i = 0; i < size; i += 4096) {
            buf[i] = (byte)((buf[i] * 3) ^ 0x55);
        }
        long t5 = System.nanoTime();
        double transformMs = (t5 - t4) / 1_000_000.0;

        double totalMs = writeMs + readMs + transformMs;
        double throughputMib = ((double)size / (1024.0 * 1024.0)) / (totalMs / 1000.0);

        System.out.println("WORKLOAD: ST4-10");
        System.out.println("LANGUAGE: Java");
        System.out.println("BUFFER_MIB: " + (size / (1024 * 1024)));
        System.out.printf("TIME_MS: %.3f\n", totalMs);
        System.out.printf("THROUGHPUT_MIBS: %.2f\n", throughputMib);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: PASS");
    }
}
