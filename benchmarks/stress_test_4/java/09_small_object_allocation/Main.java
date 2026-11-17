public class Main {
    static class Node {
        long a;
        long b;
        Node next;
        Node(long a, long b, Node next) {
            this.a = a;
            this.b = b;
            this.next = next;
        }
    }

    public static void main(String[] args) {
        int n = 10_000_000;
        if (args.length > 0) {
            n = Integer.parseInt(args[0]);
        }

        long t0 = System.nanoTime();
        Node head = null;
        Node curr = null;
        for (int i = 0; i < n; i++) {
            Node node = new Node(i, i * 2L, null);
            if (head == null) {
                head = node;
                curr = node;
            } else {
                curr.next = node;
                curr = node;
            }
        }
        long t1 = System.nanoTime();
        double allocMs = (t1 - t0) / 1_000_000.0;

        long t2 = System.nanoTime();
        long checksum = 0L;
        Node walker = head;
        while (walker != null) {
            checksum += walker.a + walker.b;
            walker = walker.next;
        }
        long t3 = System.nanoTime();
        double traverseMs = (t3 - t2) / 1_000_000.0;

        long t4 = System.nanoTime();
        head = null;
        curr = null;
        walker = null;
        System.gc();
        long t5 = System.nanoTime();
        double reclaimMs = (t5 - t4) / 1_000_000.0;

        double totalMs = allocMs + traverseMs + reclaimMs;
        System.out.println("WORKLOAD: ST4-09");
        System.out.println("LANGUAGE: Java");
        System.out.println("OBJECTS: " + n);
        System.out.printf("ALLOC_MS: %.3f\n", allocMs);
        System.out.printf("TRAVERSE_MS: %.3f\n", traverseMs);
        System.out.printf("RECLAIM_MS: %.3f\n", reclaimMs);
        System.out.printf("TIME_MS: %.3f\n", totalMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: PASS");
    }
}
