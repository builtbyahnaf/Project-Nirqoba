#include <stdio.h>
#include <stdlib.h>

int main() {
    int n = 512;
    int size = n * n;
    double *A = (double*)malloc(size * sizeof(double));
    double *B = (double*)malloc(size * sizeof(double));
    double *C = (double*)malloc(size * sizeof(double));

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            A[i*n+j] = (i + j) * 0.001;
            B[i*n+j] = (i - j) * 0.001;
            C[i*n+j] = 0.0;
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
    free(A); free(B); free(C);
    return 0;
}
