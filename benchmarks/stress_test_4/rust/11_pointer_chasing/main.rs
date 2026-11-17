use std::env;
use std::time::Instant;

fn main() {
    let mut n: usize = 10_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
    }

    let mut next_idx: Vec<usize> = (0..n).collect();
    let mut state = 0xDEADBEEFu32;
    for i in (1..n).rev() {
        state = state.wrapping_mul(1664525).wrapping_add(1013904223);
        let j = (state as usize) % (i + 1);
        next_idx.swap(i, j);
    }

    let t0 = Instant::now();
    let mut curr = 0usize;
    let mut checksum: u64 = 0;
    for _ in 0..n {
        checksum += curr as u64;
        curr = next_idx[curr];
    }
    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-11");
    println!("LANGUAGE: Rust");
    println!("NODES: {}", n);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {}", checksum);
    println!("STATUS: PASS");
}
