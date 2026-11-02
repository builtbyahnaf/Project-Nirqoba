#include <cstdio>

long long fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    printf("%lld\n", fib(40));
    return 0;
}
