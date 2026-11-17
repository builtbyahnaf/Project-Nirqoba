import java.nio.file.Files;
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
        while (pos < len && buffer[pos] != (byte)'\n') pos++;
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

                if (!inQuotes && (b == (byte)',' || b == (byte)'\n' || b == (byte)'\r')) {
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
                    if (b == (byte)'\r' && pos < len && buffer[pos] == (byte)'\n') pos++;
                    if (b == (byte)'\n' || b == (byte)'\r') break;
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
        System.out.printf("TIME_MS: %.3f\n", parseMs);
        System.out.println("SUM_ID: " + sumId);
        System.out.println("SUM_DEVICE: " + sumDeviceCode);
        System.out.println("EMPTY_CAT: " + emptyCatCount);
        System.out.println("CHECKSUM: " + (sumId + sumDeviceCode));
        System.out.println("STATUS: " + (totalRows == 5000000L ? "PASS" : "FAIL"));
    }
}
