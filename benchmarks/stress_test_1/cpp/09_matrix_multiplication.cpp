#include <cstdio>
#include <vector>

int main() {
    int n = 512;
    int size = n * n;
    std::vector<double> A(size), B(size), C(size, 0.0);

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            A[i*n+j] = (i + j) * 0.001;
            B[i*n+j] = (i - j) * 0.001;
        }
    }
    for (int r = 0; r < n; r++) {
        for (int k = 0; k < n; k++) {
            double aik = A[r*n+k];
            for (int c = 0; c < n; c++) {
                C[r*n+c] += aik * B[k*n+c];
            }
        }
    }
    double check = C[0] + C[n*256+256] + C[n*511+511];
    printf("%.6f\n", check);
    return 0;
}
