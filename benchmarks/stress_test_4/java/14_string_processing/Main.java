public class Main {
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
            if (c == ' ' || c == '\n' || c == '\t') {
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
        System.out.printf("CHAR_COUNT_MS: %.3f\n", charMs);
        System.out.printf("SUBSTRING_MS: %.3f\n", subMs);
        System.out.printf("TOKENIZE_MS: %.3f\n", tokMs);
        System.out.printf("CASE_CONV_MS: %.3f\n", caseMs);
        System.out.printf("TIME_MS: %.3f\n", totalMs);
        System.out.println("CHECKSUM: " + (charCount + subCount + tokenCount));
        System.out.println("STATUS: PASS");
    }
}
