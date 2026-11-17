use std::env;
use std::time::Instant;
use std::num::Wrapping;

fn main() {
    let mut total_ops: u64 = 500_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        total_ops = args[1].parse().unwrap_or(total_ops);
    }

    let t0 = Instant::now();
    let mut total = Wrapping(0u64);

    for i in 1..=total_ops {
        total += Wrapping(i);
        total -= Wrapping(i & 0xFF);
        total *= Wrapping(3u64);
        let div = (i % 7) + 1;
        total = total ^ (total / Wrapping(div));
        total = total ^ (total << 5) ^ (total >> 59);
    }

    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-01");
    println!("LANGUAGE: Rust");
    println!("OPS: {}", total_ops);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {}", total.0);
    println!("STATUS: PASS");
}
