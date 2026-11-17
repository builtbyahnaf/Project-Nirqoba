use std::env;
use std::time::Instant;

fn merge_sort_recursive(arr: &mut [i32], temp: &mut [i32], left: usize, right: usize) {
    if left >= right {
        return;
    }
    let mid = left + ((right - left) >> 1);
    merge_sort_recursive(arr, temp, left, mid);
    merge_sort_recursive(arr, temp, mid + 1, right);

    let mut i = left;
    let mut j = mid + 1;
    let mut k = left;
    while i <= mid && j <= right {
        if arr[i] <= arr[j] {
            temp[k] = arr[i];
            i += 1;
        } else {
            temp[k] = arr[j];
            j += 1;
        }
        k += 1;
    }
    while i <= mid {
        temp[k] = arr[i];
        i += 1;
        k += 1;
    }
    while j <= right {
        temp[k] = arr[j];
        j += 1;
        k += 1;
    }
    for p in left..=right {
        arr[p] = temp[p];
    }
}

fn main() {
    let mut n: usize = 4_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
    }

    let mut arr = Vec::with_capacity(n);
    let mut temp = vec![0i32; n];

    let mut state = 0x54321u32;
    for _ in 0..n {
        state = state.wrapping_mul(1664525).wrapping_add(1013904223);
        arr.push(state as i32);
    }

    let t0 = Instant::now();
    merge_sort_recursive(&mut arr, &mut temp, 0, n - 1);
    let sort_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let is_sorted = arr[0] <= arr[n - 1];
    let mut checksum: u64 = 0;
    for i in (0..n).step_by(1000) {
        checksum = checksum.wrapping_add(arr[i] as u64);
    }

    println!("WORKLOAD: ST4-13");
    println!("LANGUAGE: Rust");
    println!("ELEMENTS: {}", n);
    println!("TIME_MS: {:.3}", sort_ms);
    println!("CHECKSUM: {}", checksum);
    println!("STATUS: {}", if is_sorted { "PASS" } else { "FAIL" });
}
