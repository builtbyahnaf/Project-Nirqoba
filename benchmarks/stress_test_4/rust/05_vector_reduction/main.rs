use std::env;
use std::time::Instant;

fn main() {
    let mut n: usize = 50_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
    }

    let t0 = Instant::now();
    let mut vec = Vec::with_capacity(n);
    for i in 0..n {
        vec.push((i as u64) ^ 0x55555555u64);
    }
    let construct_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let t1 = Instant::now();
    let total: u64 = vec.iter().sum();
    let reduce_ms = t1.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-05");
    println!("LANGUAGE: Rust");
    println!("ELEMENTS: {}", n);
    println!("CONSTRUCTION_MS: {:.3}", construct_ms);
    println!("REDUCTION_MS: {:.3}", reduce_ms);
    println!("TIME_MS: {:.3}", construct_ms + reduce_ms);
    println!("CHECKSUM: {}", total);
    println!("STATUS: PASS");
}
