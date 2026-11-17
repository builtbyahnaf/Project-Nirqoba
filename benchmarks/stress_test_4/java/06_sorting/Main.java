import java.util.Arrays;

public class Main {
    public static void main(String[] args) {
        int n = 5_000_000;
        if (args.length > 0) {
            n = Integer.parseInt(args[0]);
        }

        int[] arr = new int[n];
        int state = 0x12345678;
        for (int i = 0; i < n; i++) {
            state = state * 1664525 + 1013904223;
            arr[i] = state;
        }

        long t0 = System.nanoTime();
        Arrays.sort(arr);
        long t1 = System.nanoTime();
        double sortMs = (t1 - t0) / 1_000_000.0;

        boolean isSorted = true;
        for (int i = 0; i < Math.min(n - 1, 10000); i++) {
            if (arr[i] > arr[i + 1]) {
                isSorted = false;
                break;
            }
        }

        long checksum = 0L;
        for (int i = 0; i < n; i += 1000) {
            checksum += arr[i];
        }

        System.out.println("WORKLOAD: ST4-06");
        System.out.println("LANGUAGE: Java");
        System.out.println("ELEMENTS: " + n);
        System.out.printf("TIME_MS: %.3f\n", sortMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: " + (isSorted ? "PASS" : "FAIL"));
    }
}
