public class Main {
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
        System.out.printf("TIME_MS: %.3f\n", timeMs);
        System.out.printf("CHECKSUM: %.6f\n", checksum);
        System.out.println("STATUS: PASS");
    }
}
