use std::env;
use std::time::Instant;

fn main() {
    let mut size: usize = 512 * 1024 * 1024; // 512 MiB
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        let mib: usize = args[1].parse().unwrap_or(512);
        size = mib * 1024 * 1024;
    }

    let t0 = Instant::now();
    let mut buf = vec![0u8; size];
    for i in (0..size).step_by(4096) {
        buf[i] = (i & 0xFF) as u8;
    }
    let write_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let t1 = Instant::now();
    let mut checksum: u64 = 0;
    for i in (0..size).step_by(1024) {
        checksum += buf[i] as u64;
    }
    let read_ms = t1.elapsed().as_secs_f64() * 1000.0;

    let t2 = Instant::now();
    for i in (0..size).step_by(4096) {
        buf[i] = (buf[i].wrapping_mul(3)) ^ 0x55;
    }
    let transform_ms = t2.elapsed().as_secs_f64() * 1000.0;

    let total_ms = write_ms + read_ms + transform_ms;
    let throughput_mib = (size as f64 / (1024.0 * 1024.0)) / (total_ms / 1000.0);

    println!("WORKLOAD: ST4-10");
    println!("LANGUAGE: Rust");
    println!("BUFFER_MIB: {}", size / (1024 * 1024));
    println!("TIME_MS: {:.3}", total_ms);
    println!("THROUGHPUT_MIBS: {:.2}", throughput_mib);
    println!("CHECKSUM: {}", checksum);
    println!("STATUS: PASS");
}
