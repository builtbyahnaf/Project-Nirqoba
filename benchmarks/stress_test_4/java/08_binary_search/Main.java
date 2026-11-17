import java.util.Arrays;

public class Main {
    public static void main(String[] args) {
        int n = 20_000_000;
        int queries = 10_000_000;
        if (args.length > 0) {
            n = Integer.parseInt(args[0]);
            queries = n / 2;
        }

        long[] arr = new long[n];
        for (int i = 0; i < n; i++) {
            arr[i] = ((long)i) * 2;
        }

        long t0 = System.nanoTime();
        long hitCount = 0L;
        long checksum = 0L;
        long maxVal = ((long)n) * 2;

        for (int q = 0; q < queries; q++) {
            long target = (((long)q) * 3) % maxVal;
            int idx = Arrays.binarySearch(arr, target);
            if (idx >= 0) {
                hitCount++;
                checksum += idx;
            } else {
                checksum -= 1;
            }
        }
        long t1 = System.nanoTime();
        double timeMs = (t1 - t0) / 1_000_000.0;

        System.out.println("WORKLOAD: ST4-08");
        System.out.println("LANGUAGE: Java");
        System.out.println("ARRAY_SIZE: " + n);
        System.out.println("QUERIES: " + queries);
        System.out.println("HITS: " + hitCount);
        System.out.printf("TIME_MS: %.3f\n", timeMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: PASS");
    }
}
