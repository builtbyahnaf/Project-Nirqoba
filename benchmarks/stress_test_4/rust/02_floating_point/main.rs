use std::env;
use std::time::Instant;

fn main() {
    let mut total_ops: u64 = 100_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        total_ops = args[1].parse().unwrap_or(total_ops);
    }

    let t0 = Instant::now();
    let mut acc: f64 = 1.0;

    for _ in 1..=total_ops {
        acc = acc + 0.001;
        acc = acc * 1.000001;
        acc = acc / 1.0000005;
        acc = acc + acc.sqrt() * 0.00001;
    }

    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-02");
    println!("LANGUAGE: Rust");
    println!("OPS: {}", total_ops);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {:.6}", acc);
    println!("STATUS: PASS");
}
