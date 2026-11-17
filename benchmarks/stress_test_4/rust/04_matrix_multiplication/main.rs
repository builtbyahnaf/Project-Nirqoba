use std::env;
use std::time::Instant;

fn main() {
    let mut size: usize = 1024;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        size = args[1].parse().unwrap_or(size);
    }

    let total = size * size;
    let mut a = vec![0.0f64; total];
    let mut b = vec![0.0f64; total];
    let mut c = vec![0.0f64; total];

    for i in 0..size {
        for j in 0..size {
            a[i * size + j] = ((i + j) % 100) as f64 / 100.0;
            b[i * size + j] = ((i * j) % 100) as f64 / 100.0;
        }
    }

    let t0 = Instant::now();
    for i in 0..size {
        let i_off = i * size;
        for k in 0..size {
            let a_ik = a[i_off + k];
            let k_off = k * size;
            for j in 0..size {
                c[i_off + j] += a_ik * b[k_off + j];
            }
        }
    }
    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let checksum: f64 = c.iter().sum();

    println!("WORKLOAD: ST4-04");
    println!("LANGUAGE: Rust");
    println!("MATRIX_DIM: {}x{}", size, size);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {:.6}", checksum);
    println!("STATUS: PASS");
}
