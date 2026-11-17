use std::env;
use std::time::Instant;

fn main() {
    let mut n: usize = 20_000_000;
    let mut queries: usize = 10_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
        queries = n / 2;
    }

    let mut arr = Vec::with_capacity(n);
    for i in 0..n {
        arr.push((i as i64) * 2);
    }

    let t0 = Instant::now();
    let mut hit_count: i64 = 0;
    let mut checksum: i64 = 0;
    let max_val = (n as i64) * 2;

    for q in 0..queries {
        let target = ((q as i64) * 3) % max_val;
        match arr.binary_search(&target) {
            Ok(idx) => {
                hit_count += 1;
                checksum += idx as i64;
            }
            Err(_) => {
                checksum -= 1;
            }
        }
    }
    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-08");
    println!("LANGUAGE: Rust");
    println!("ARRAY_SIZE: {}", n);
    println!("QUERIES: {}", queries);
    println!("HITS: {}", hit_count);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {}", checksum);
    println!("STATUS: PASS");
}
