#include <cstdio>

int main() {
    double sum = 0.0, sign = 1.0;
    for (int i = 0; i < 100000000; i++) {
        sum += sign / (2.0 * i + 1.0);
        sign = -sign;
    }
    double pi = sum * 4.0;
    printf("%.14f\n", pi);
    return 0;
}
