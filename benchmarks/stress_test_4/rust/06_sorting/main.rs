use std::env;
use std::time::Instant;

fn main() {
    let mut n: usize = 5_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
    }

    let mut arr = Vec::with_capacity(n);
    let mut state = 0x12345678u32;
    for _ in 0..n {
        state = state.wrapping_mul(1664525).wrapping_add(1013904223);
        arr.push(state as i32);
    }

    let t0 = Instant::now();
    arr.sort_unstable();
    let sort_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let is_sorted = arr.windows(2).take(10000).all(|w| w[0] <= w[1]);
    let mut checksum: u64 = 0;
    for i in (0..n).step_by(1000) {
        checksum = checksum.wrapping_add(arr[i] as u64);
    }

    println!("WORKLOAD: ST4-06");
    println!("LANGUAGE: Rust");
    println!("ELEMENTS: {}", n);
    println!("TIME_MS: {:.3}", sort_ms);
    println!("CHECKSUM: {}", checksum);
    println!("STATUS: {}", if is_sorted { "PASS" } else { "FAIL" });
}
