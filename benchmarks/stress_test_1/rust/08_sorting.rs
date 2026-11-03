fn qsort(arr: &mut [i64], low: usize, high: usize) {
    let mut l = low;
    let mut h = high;
    while l < h {
        let mid = l + (h - l) / 2;
        let pivot = arr[mid];
        let mut i = l;
        let mut j = h;
        while i <= j {
            while arr[i] < pivot { i += 1; }
            while arr[j] > pivot {
                if j == 0 { break; }
                j -= 1;
            }
            if i <= j {
                arr.swap(i, j);
                i += 1;
                if j == 0 { break; }
                j -= 1;
            }
        }
        if j.saturating_sub(l) < h.saturating_sub(i) {
            if l < j { qsort(arr, l, j); }
            l = i;
        } else {
            if i < h { qsort(arr, i, h); }
            if j == 0 { break; }
            h = j;
        }
    }
}

fn main() {
    let size = 1_000_000usize;
    let mut arr: Vec<i64> = Vec::with_capacity(size);
    let mut seed: i64 = 123456789;
    for _ in 0..size {
        seed = (seed.wrapping_mul(1103515245).wrapping_add(12345)) & 2147483647;
        arr.push(seed);
    }
    qsort(&mut arr, 0, size - 1);
    println!("{}", arr[0]);
    println!("{}", arr[size / 2]);
    println!("{}", arr[size - 1]);
}
