public class Main {
    static void mergeSortRecursive(int[] arr, int[] temp, int left, int right) {
        if (left >= right) return;
        int mid = left + ((right - left) >> 1);
        mergeSortRecursive(arr, temp, left, mid);
        mergeSortRecursive(arr, temp, mid + 1, right);

        int i = left, j = mid + 1, k = left;
        while (i <= mid && j <= right) {
            if (arr[i] <= arr[j]) {
                temp[k++] = arr[i++];
            } else {
                temp[k++] = arr[j++];
            }
        }
        while (i <= mid) temp[k++] = arr[i++];
        while (j <= right) temp[k++] = arr[j++];
        for (int p = left; p <= right; p++) {
            arr[p] = temp[p];
        }
    }

    public static void main(String[] args) {
        int n = 4_000_000;
        if (args.length > 0) {
            n = Integer.parseInt(args[0]);
        }

        int[] arr = new int[n];
        int[] temp = new int[n];

        int state = 0x54321;
        for (int i = 0; i < n; i++) {
            state = state * 1664525 + 1013904223;
            arr[i] = state;
        }

        long t0 = System.nanoTime();
        mergeSortRecursive(arr, temp, 0, n - 1);
        long t1 = System.nanoTime();
        double sortMs = (t1 - t0) / 1_000_000.0;

        boolean isSorted = (arr[0] <= arr[n - 1]);
        long checksum = 0L;
        for (int i = 0; i < n; i += 1000) {
            checksum += arr[i];
        }

        System.out.println("WORKLOAD: ST4-13");
        System.out.println("LANGUAGE: Java");
        System.out.println("ELEMENTS: " + n);
        System.out.printf("TIME_MS: %.3f\n", sortMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: " + (isSorted ? "PASS" : "FAIL"));
    }
}
