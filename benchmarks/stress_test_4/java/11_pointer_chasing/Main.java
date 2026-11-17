public class Main {
    public static void main(String[] args) {
        int n = 10_000_000;
        if (args.length > 0) {
            n = Integer.parseInt(args[0]);
        }

        int[] nextIdx = new int[n];
        for (int i = 0; i < n; i++) {
            nextIdx[i] = i;
        }

        int state = 0xDEADBEEF;
        for (int i = n - 1; i > 0; i--) {
            state = state * 1664525 + 1013904223;
            int uState = state & 0x7FFFFFFF;
            int j = uState % (i + 1);
            int tmp = nextIdx[i];
            nextIdx[i] = nextIdx[j];
            nextIdx[j] = tmp;
        }

        long t0 = System.nanoTime();
        int curr = 0;
        long checksum = 0L;
        for (int i = 0; i < n; i++) {
            checksum += curr;
            curr = nextIdx[curr];
        }
        long t1 = System.nanoTime();
        double timeMs = (t1 - t0) / 1_000_000.0;

        System.out.println("WORKLOAD: ST4-11");
        System.out.println("LANGUAGE: Java");
        System.out.println("NODES: " + n);
        System.out.printf("TIME_MS: %.3f\n", timeMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: PASS");
    }
}
