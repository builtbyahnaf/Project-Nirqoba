#include <cstdio>
#include <cstdint>
#include <vector>

void do_swap(std::vector<int64_t>& arr, int i, int j) {
    int64_t t = arr[i]; arr[i] = arr[j]; arr[j] = t;
}

void qsort_impl(std::vector<int64_t>& arr, int low, int high) {
    int l = low, h = high;
    while (l < h) {
        int mid = l + (h - l) / 2;
        int64_t pivot = arr[mid];
        int i = l, j = h;
        while (i <= j) {
            while (arr[i] < pivot) i++;
            while (arr[j] > pivot) j--;
            if (i <= j) { do_swap(arr, i, j); i++; j--; }
        }
        if (j - l < h - i) {
            if (l < j) qsort_impl(arr, l, j);
            l = i;
        } else {
            if (i < h) qsort_impl(arr, i, h);
            h = j;
        }
    }
}

int main() {
    int size = 1000000;
    std::vector<int64_t> arr(size);
    int64_t seed = 123456789;
    for (int i = 0; i < size; i++) {
        seed = (seed * 1103515245LL + 12345) & 2147483647LL;
        arr[i] = seed;
    }
    qsort_impl(arr, 0, size - 1);
    printf("%lld\n", arr[0]);
    printf("%lld\n", arr[size / 2]);
    printf("%lld\n", arr[size - 1]);
    return 0;
}
