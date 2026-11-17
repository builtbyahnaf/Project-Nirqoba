import java.util.HashMap;

public class Main {
    public static void main(String[] args) {
        int n = 5_000_000;
        int removals = 1_000_000;
        if (args.length > 0) {
            n = Integer.parseInt(args[0]);
            removals = n / 5;
        }

        HashMap<Long, Long> map = new HashMap<>(n);

        long t0 = System.nanoTime();
        for (int i = 0; i < n; i++) {
            long key = ((long)i) * 2654435761L;
            map.put(key, (long)i);
        }
        long t1 = System.nanoTime();
        double insertMs = (t1 - t0) / 1_000_000.0;

        long t2 = System.nanoTime();
        long lookupSum = 0L;
        for (int i = 0; i < n; i++) {
            long key = ((long)i) * 2654435761L;
            lookupSum += map.get(key);
        }
        long t3 = System.nanoTime();
        double lookupMs = (t3 - t2) / 1_000_000.0;

        long t4 = System.nanoTime();
        for (int i = 0; i < removals * 5; i += 5) {
            long key = ((long)i) * 2654435761L;
            map.remove(key);
        }
        long t5 = System.nanoTime();
        double removeMs = (t5 - t4) / 1_000_000.0;

        double totalMs = insertMs + lookupMs + removeMs;
        System.out.println("WORKLOAD: ST4-07");
        System.out.println("LANGUAGE: Java");
        System.out.printf("INSERT_MS: %.3f\n", insertMs);
        System.out.printf("LOOKUP_MS: %.3f\n", lookupMs);
        System.out.printf("REMOVE_MS: %.3f\n", removeMs);
        System.out.printf("TIME_MS: %.3f\n", totalMs);
        System.out.println("REMAINING: " + map.size());
        System.out.println("CHECKSUM: " + lookupSum);
        System.out.println("STATUS: " + (map.size() == (n - removals) ? "PASS" : "FAIL"));
    }
}
