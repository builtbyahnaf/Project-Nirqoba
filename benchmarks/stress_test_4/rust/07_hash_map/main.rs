use std::env;
use std::time::Instant;
use std::collections::HashMap;

fn main() {
    let mut n: usize = 5_000_000;
    let mut removals: usize = 1_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
        removals = n / 5;
    }

    let mut map = HashMap::with_capacity(n);

    let t0 = Instant::now();
    for i in 0..n {
        let key = (i as u64).wrapping_mul(2654435761u64);
        map.insert(key, i as u64);
    }
    let insert_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let t1 = Instant::now();
    let mut lookup_sum: u64 = 0;
    for i in 0..n {
        let key = (i as u64).wrapping_mul(2654435761u64);
        if let Some(&val) = map.get(&key) {
            lookup_sum += val;
        }
    }
    let lookup_ms = t1.elapsed().as_secs_f64() * 1000.0;

    let t2 = Instant::now();
    for i in (0..removals * 5).step_by(5) {
        let key = (i as u64).wrapping_mul(2654435761u64);
        map.remove(&key);
    }
    let remove_ms = t2.elapsed().as_secs_f64() * 1000.0;

    let total_ms = insert_ms + lookup_ms + remove_ms;
    println!("WORKLOAD: ST4-07");
    println!("LANGUAGE: Rust");
    println!("INSERT_MS: {:.3}", insert_ms);
    println!("LOOKUP_MS: {:.3}", lookup_ms);
    println!("REMOVE_MS: {:.3}", remove_ms);
    println!("TIME_MS: {:.3}", total_ms);
    println!("REMAINING: {}", map.len());
    println!("CHECKSUM: {}", lookup_sum);
    println!("STATUS: {}", if map.len() == (n - removals) { "PASS" } else { "FAIL" });
}
