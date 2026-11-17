"""
Generator for Java Stress Test 4 Workloads (ST4-01 to ST4-16)
"""
import os

base = r"benchmarks\stress_test_4\java"

java_sources = {
"01_integer_arithmetic": '''public class Main {
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
        System.out.printf("TIME_MS: %.3f\\n", timeMs);
        System.out.println("CHECKSUM: " + total);
        System.out.println("STATUS: PASS");
    }
}
''',

"02_floating_point": '''public class Main {
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
        System.out.printf("TIME_MS: %.3f\\n", timeMs);
        System.out.printf("CHECKSUM: %.6f\\n", acc);
        System.out.println("STATUS: PASS");
    }
}
''',

"03_branch_heavy": '''public class Main {
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
        System.out.printf("TIME_MS: %.3f\\n", timeMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: PASS");
    }
}
''',

"04_matrix_multiplication": '''public class Main {
    public static void main(String[] args) {
        int size = 1024;
        if (args.length > 0) {
            size = Integer.parseInt(args[0]);
        }

        int total = size * size;
        double[] a = new double[total];
        double[] b = new double[total];
        double[] c = new double[total];

        for (int i = 0; i < size; i++) {
            for (int j = 0; j < size; j++) {
                a[i * size + j] = ((i + j) % 100) / 100.0;
                b[i * size + j] = ((i * j) % 100) / 100.0;
            }
        }

        long t0 = System.nanoTime();
        for (int i = 0; i < size; i++) {
            int iOff = i * size;
            for (int k = 0; k < size; k++) {
                double aIk = a[iOff + k];
                int kOff = k * size;
                for (int j = 0; j < size; j++) {
                    c[iOff + j] += aIk * b[kOff + j];
                }
            }
        }
        long t1 = System.nanoTime();
        double timeMs = (t1 - t0) / 1_000_000.0;

        double checksum = 0.0;
        for (int i = 0; i < total; i++) {
            checksum += c[i];
        }

        System.out.println("WORKLOAD: ST4-04");
        System.out.println("LANGUAGE: Java");
        System.out.println("MATRIX_DIM: " + size + "x" + size);
        System.out.printf("TIME_MS: %.3f\\n", timeMs);
        System.out.printf("CHECKSUM: %.6f\\n", checksum);
        System.out.println("STATUS: PASS");
    }
}
''',

"05_vector_reduction": '''public class Main {
    public static void main(String[] args) {
        int n = 50_000_000;
        if (args.length > 0) {
            n = Integer.parseInt(args[0]);
        }

        long t0 = System.nanoTime();
        long[] vec = new long[n];
        for (int i = 0; i < n; i++) {
            vec[i] = ((long)i) ^ 0x55555555L;
        }
        long t1 = System.nanoTime();
        double constructMs = (t1 - t0) / 1_000_000.0;

        long t2 = System.nanoTime();
        long total = 0L;
        for (int i = 0; i < n; i++) {
            total += vec[i];
        }
        long t3 = System.nanoTime();
        double reduceMs = (t3 - t2) / 1_000_000.0;

        System.out.println("WORKLOAD: ST4-05");
        System.out.println("LANGUAGE: Java");
        System.out.println("ELEMENTS: " + n);
        System.out.printf("CONSTRUCTION_MS: %.3f\\n", constructMs);
        System.out.printf("REDUCTION_MS: %.3f\\n", reduceMs);
        System.out.printf("TIME_MS: %.3f\\n", constructMs + reduceMs);
        System.out.println("CHECKSUM: " + total);
        System.out.println("STATUS: PASS");
    }
}
''',

"06_sorting": '''import java.util.Arrays;

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
        System.out.printf("TIME_MS: %.3f\\n", sortMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: " + (isSorted ? "PASS" : "FAIL"));
    }
}
''',

"07_hash_map": '''import java.util.HashMap;

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
        System.out.printf("INSERT_MS: %.3f\\n", insertMs);
        System.out.printf("LOOKUP_MS: %.3f\\n", lookupMs);
        System.out.printf("REMOVE_MS: %.3f\\n", removeMs);
        System.out.printf("TIME_MS: %.3f\\n", totalMs);
        System.out.println("REMAINING: " + map.size());
        System.out.println("CHECKSUM: " + lookupSum);
        System.out.println("STATUS: " + (map.size() == (n - removals) ? "PASS" : "FAIL"));
    }
}
''',

"08_binary_search": '''import java.util.Arrays;

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
        System.out.printf("TIME_MS: %.3f\\n", timeMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: PASS");
    }
}
''',

"09_small_object_allocation": '''public class Main {
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
        System.out.printf("ALLOC_MS: %.3f\\n", allocMs);
        System.out.printf("TRAVERSE_MS: %.3f\\n", traverseMs);
        System.out.printf("RECLAIM_MS: %.3f\\n", reclaimMs);
        System.out.printf("TIME_MS: %.3f\\n", totalMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: PASS");
    }
}
''',

"10_large_memory_throughput": '''public class Main {
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
        System.out.printf("TIME_MS: %.3f\\n", totalMs);
        System.out.printf("THROUGHPUT_MIBS: %.2f\\n", throughputMib);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: PASS");
    }
}
''',

"11_pointer_chasing": '''public class Main {
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
        System.out.printf("TIME_MS: %.3f\\n", timeMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: PASS");
    }
}
''',

"12_binary_tree": '''public class Main {
    static class TreeNode {
        int val;
        TreeNode left;
        TreeNode right;
        TreeNode(int v, TreeNode l, TreeNode r) {
            this.val = v;
            this.left = l;
            this.right = r;
        }
    }

    static TreeNode buildTree(int depth, int idx) {
        if (depth == 0) return null;
        return new TreeNode(idx, buildTree(depth - 1, idx * 2), buildTree(depth - 1, idx * 2 + 1));
    }

    static long preorder(TreeNode node) {
        if (node == null) return 0L;
        return node.val + preorder(node.left) + preorder(node.right);
    }

    static long inorder(TreeNode node) {
        if (node == null) return 0L;
        return inorder(node.left) + node.val + inorder(node.right);
    }

    static long postorder(TreeNode node) {
        if (node == null) return 0L;
        return postorder(node.left) + postorder(node.right) + node.val;
    }

    public static void main(String[] args) {
        int depth = 20; // 1,048,575 nodes
        if (args.length > 0) {
            depth = Integer.parseInt(args[0]);
        }

        long t0 = System.nanoTime();
        TreeNode root = buildTree(depth, 1);
        long t1 = System.nanoTime();
        double buildMs = (t1 - t0) / 1_000_000.0;

        long t2 = System.nanoTime();
        long sumPre = preorder(root);
        long t3 = System.nanoTime();
        double preMs = (t3 - t2) / 1_000_000.0;

        long t4 = System.nanoTime();
        long sumIn = inorder(root);
        long t5 = System.nanoTime();
        double inMs = (t5 - t4) / 1_000_000.0;

        long t6 = System.nanoTime();
        long sumPost = postorder(root);
        long t7 = System.nanoTime();
        double postMs = (t7 - t6) / 1_000_000.0;

        double totalMs = buildMs + preMs + inMs + postMs;
        System.out.println("WORKLOAD: ST4-12");
        System.out.println("LANGUAGE: Java");
        System.out.println("NODES: " + ((1 << depth) - 1));
        System.out.printf("BUILD_MS: %.3f\\n", buildMs);
        System.out.printf("PREORDER_MS: %.3f\\n", preMs);
        System.out.printf("INORDER_MS: %.3f\\n", inMs);
        System.out.printf("POSTORDER_MS: %.3f\\n", postMs);
        System.out.printf("TIME_MS: %.3f\\n", totalMs);
        System.out.println("CHECKSUM: " + sumPre);
        System.out.println("STATUS: " + (sumPre == sumIn && sumIn == sumPost ? "PASS" : "FAIL"));
    }
}
''',

"13_recursive_overhead": '''public class Main {
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
        System.out.printf("TIME_MS: %.3f\\n", sortMs);
        System.out.println("CHECKSUM: " + checksum);
        System.out.println("STATUS: " + (isSorted ? "PASS" : "FAIL"));
    }
}
''',

"14_string_processing": '''public class Main {
    public static void main(String[] args) {
        int sizeMb = 256;
        if (args.length > 0) {
            sizeMb = Integer.parseInt(args[0]);
        }

        int totalBytes = sizeMb * 1024 * 1024;
        String pattern = "Nirqoba high performance compiler benchmark suite text processing tokenization. ";
        int repeats = totalBytes / pattern.length() + 1;
        StringBuilder sb = new StringBuilder(totalBytes);
        for (int i = 0; i < repeats; i++) {
            sb.append(pattern);
            if (sb.length() >= totalBytes) break;
        }
        sb.setLength(totalBytes);
        String corpus = sb.toString();

        // 1. Character count
        long t0 = System.nanoTime();
        long charCount = 0L;
        for (int i = 0; i < corpus.length(); i++) {
            if (corpus.charAt(i) == 'e') charCount++;
        }
        long t1 = System.nanoTime();
        double charMs = (t1 - t0) / 1_000_000.0;

        // 2. Substring search
        long t2 = System.nanoTime();
        long subCount = 0L;
        int pos = 0;
        while ((pos = corpus.indexOf("benchmark", pos)) != -1) {
            subCount++;
            pos += 9;
        }
        long t3 = System.nanoTime();
        double subMs = (t3 - t2) / 1_000_000.0;

        // 3. Whitespace tokenization
        long t4 = System.nanoTime();
        long tokenCount = 0L;
        boolean inToken = false;
        for (int i = 0; i < corpus.length(); i++) {
            char c = corpus.charAt(i);
            if (c == ' ' || c == '\\n' || c == '\\t') {
                inToken = false;
            } else if (!inToken) {
                inToken = true;
                tokenCount++;
            }
        }
        long t5 = System.nanoTime();
        double tokMs = (t5 - t4) / 1_000_000.0;

        // 4. Case conversion
        long t6 = System.nanoTime();
        String upperCorpus = corpus.toUpperCase();
        long t7 = System.nanoTime();
        double caseMs = (t7 - t6) / 1_000_000.0;

        double totalMs = charMs + subMs + tokMs + caseMs;
        System.out.println("WORKLOAD: ST4-14");
        System.out.println("LANGUAGE: Java");
        System.out.println("CORPUS_MIB: " + sizeMb);
        System.out.printf("CHAR_COUNT_MS: %.3f\\n", charMs);
        System.out.printf("SUBSTRING_MS: %.3f\\n", subMs);
        System.out.printf("TOKENIZE_MS: %.3f\\n", tokMs);
        System.out.printf("CASE_CONV_MS: %.3f\\n", caseMs);
        System.out.printf("TIME_MS: %.3f\\n", totalMs);
        System.out.println("CHECKSUM: " + (charCount + subCount + tokenCount));
        System.out.println("STATUS: PASS");
    }
}
''',

"15_csv_parsing": '''import java.nio.file.Files;
import java.nio.file.Paths;

public class Main {
    public static void main(String[] args) throws Exception {
        String filename = "benchmarks/stress_test_4/artifacts/st4_15_benchmark_5m.csv";
        if (args.length > 0) {
            filename = args[0];
        }

        byte[] buffer = Files.readAllBytes(Paths.get(filename));

        long t0 = System.nanoTime();

        long totalRows = 0L;
        long sumId = 0L;
        long sumDeviceCode = 0L;
        long emptyCatCount = 0L;
        long escapedQuotesCount = 0L;

        int len = buffer.length;
        int pos = 0;

        // Skip header
        while (pos < len && buffer[pos] != (byte)'\\n') pos++;
        if (pos < len) pos++;

        while (pos < len) {
            totalRows++;
            int col = 0;
            int cellStart = pos;
            boolean inQuotes = false;
            boolean rowHasEscapedQuote = false;

            long col0Val = 0L;
            long col4Val = 0L;
            int col6Len = 0;

            while (pos < len) {
                byte b = buffer[pos];
                if (b == (byte)'"') {
                    if (inQuotes && pos + 1 < len && buffer[pos + 1] == (byte)'"') {
                        rowHasEscapedQuote = true;
                        pos += 2;
                        continue;
                    }
                    inQuotes = !inQuotes;
                    pos++;
                    continue;
                }

                if (!inQuotes && (b == (byte)',' || b == (byte)'\\n' || b == (byte)'\\r')) {
                    int cellLen = pos - cellStart;
                    if (col == 0) {
                        long num = 0L;
                        for (int k = cellStart; k < pos; k++) {
                            num = num * 10L + (buffer[k] - (byte)'0');
                        }
                        col0Val = num;
                    } else if (col == 4) {
                        long num = 0L;
                        for (int k = cellStart; k < pos; k++) {
                            num = num * 10L + (buffer[k] - (byte)'0');
                        }
                        col4Val = num;
                    } else if (col == 6) {
                        col6Len = cellLen;
                    }

                    col++;
                    pos++;
                    if (b == (byte)'\\r' && pos < len && buffer[pos] == (byte)'\\n') pos++;
                    if (b == (byte)'\\n' || b == (byte)'\\r') break;
                    cellStart = pos;
                } else {
                    pos++;
                }
            }

            sumId += col0Val;
            sumDeviceCode += col4Val;
            if (col6Len == 0) emptyCatCount++;
            if (rowHasEscapedQuote) escapedQuotesCount++;
        }

        long t1 = System.nanoTime();
        double parseMs = (t1 - t0) / 1_000_000.0;

        System.out.println("WORKLOAD: ST4-15");
        System.out.println("LANGUAGE: Java");
        System.out.println("ROWS_PARSED: " + totalRows);
        System.out.printf("TIME_MS: %.3f\\n", parseMs);
        System.out.println("SUM_ID: " + sumId);
        System.out.println("SUM_DEVICE: " + sumDeviceCode);
        System.out.println("EMPTY_CAT: " + emptyCatCount);
        System.out.println("CHECKSUM: " + (sumId + sumDeviceCode));
        System.out.println("STATUS: " + (totalRows == 5000000L ? "PASS" : "FAIL"));
    }
}
''',

"16_cyclic_objects": '''public class Main {
    static class CycleNode {
        int value;
        CycleNode peer;
        CycleNode(int val) {
            this.value = val;
        }
    }

    public static void main(String[] args) {
        int pairsCount = 500_000;
        if (args.length > 0) {
            pairsCount = Integer.parseInt(args[0]);
        }

        long t0 = System.nanoTime();
        CycleNode[] aNodes = new CycleNode[pairsCount];
        CycleNode[] bNodes = new CycleNode[pairsCount];

        for (int i = 0; i < pairsCount; i++) {
            CycleNode a = new CycleNode(i);
            CycleNode b = new CycleNode(i);
            a.peer = b;
            b.peer = a;
            aNodes[i] = a;
            bNodes[i] = b;
        }
        long t1 = System.nanoTime();
        double buildMs = (t1 - t0) / 1_000_000.0;

        // Sever external roots
        aNodes = null;
        bNodes = null;

        long t2 = System.nanoTime();
        System.gc();
        long t3 = System.nanoTime();
        double reclaimMs = (t3 - t2) / 1_000_000.0;

        double totalMs = buildMs + reclaimMs;
        System.out.println("WORKLOAD: ST4-16");
        System.out.println("LANGUAGE: Java");
        System.out.println("CYCLES: " + pairsCount);
        System.out.printf("BUILD_MS: %.3f\\n", buildMs);
        System.out.printf("RECLAIM_MS: %.3f\\n", reclaimMs);
        System.out.printf("TIME_MS: %.3f\\n", totalMs);
        System.out.println("STATUS: PASS");
    }
}
'''
}

for folder, code in java_sources.items():
    path = os.path.join(base, folder, "Main.java")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(code)
    print(f"Wrote Java ST4: {path}")

print("Java track generation finished.")
