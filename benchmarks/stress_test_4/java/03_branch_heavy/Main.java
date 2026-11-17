public class Main {
    public static void main(String[] args) {
        long n = 100_000_000L;
        if (args.length > 0) {
            n = Long.parseLong(args[0]);
        }

        long t0 = System.nanoTime();
        int state = 123456789;
        long checksum = 0L;

        for (long i = 0; i < n; i++) {
            state = state * 1664525 + 1013904223;
            long uState = Integer.toUnsignedLong(state);
            if ((i & 1) == 0) {
                if ((uState & 1L) != 0) {
                    checksum += (uState ^ 0x55555555L);
                } else {
                    if (((uState >>> 16) & 1L) != 0) {
                        checksum += (uState ^ 0xAAAAAAAAL);
                    } else {
                        checksum += Integer.toUnsignedLong(state * 3);
                    }
                }
            } else {
                if (uState < 0x80000000L) {
                    checksum ^= (uState >>> 3);
                } else {
                    checksum -= (uState & 0xFFFFL);
                }
            }
        }

        long t1 = System.nanoTime();
        double timeMs = (t1 - t0) / 1_000_000.0;

        System.out.println("WORKLOAD: ST4-03");
        System.out.println("LANGUAGE: Java");
        System.out.println("INPUTS: " + n);
        System.out.printf("TIME_MS: %.3f\n", timeMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: PASS");
    }
}
