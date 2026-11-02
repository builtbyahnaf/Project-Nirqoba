#include <cstdio>
#include <memory>

int main() {
    int count = 0;
    for (int i = 0; i < 1000000; i++) {
        auto ptr = std::make_unique<char[]>(64);
        count++;
        // ptr auto-released at end of loop iteration
    }
    printf("%d\n", count);
    return 0;
}
