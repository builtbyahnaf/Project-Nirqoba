use std::time::Instant;
fn main() {
    let n: usize = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(1_000_000);
    let mut sum: u64 = 0;
    let start = Instant::now();
    for i in 0..n {
        let mut p = Box::new([0u64; 8]);
        p[0] = i as u64;
        p[7] = (i as u64) ^ 0x5a5a;
        sum = sum.wrapping_add(p[0] + p[7]);
    }
    println!("iterations={n} ns={} checksum={sum}", start.elapsed().as_nanos());
}
