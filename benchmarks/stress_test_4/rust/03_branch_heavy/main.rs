use std::env;
use std::time::Instant;
use std::num::Wrapping;

fn main() {
    let mut n: u64 = 100_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
    }

    let t0 = Instant::now();
    let mut state = Wrapping(123456789u32);
    let mut checksum = Wrapping(0u64);

    for i in 0..n {
        state = state * Wrapping(1664525u32) + Wrapping(1013904223u32);
        let u_val = state.0;
        if (i & 1) == 0 {
            if (u_val & 1) != 0 {
                checksum += Wrapping((u_val ^ 0x55555555) as u64);
            } else {
                if ((u_val >> 16) & 1) != 0 {
                    checksum += Wrapping((u_val ^ 0xAAAAAAAA) as u64);
                } else {
                    checksum += Wrapping((u_val.wrapping_mul(3)) as u64);
                }
            }
        } else {
            if u_val < 0x80000000 {
                checksum ^= Wrapping((u_val >> 3) as u64);
            } else {
                checksum -= Wrapping((u_val & 0xFFFF) as u64);
            }
        }
    }

    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-03");
    println!("LANGUAGE: Rust");
    println!("INPUTS: {}", n);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {}", checksum.0);
    println!("STATUS: PASS");
}
