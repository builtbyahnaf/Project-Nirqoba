fn main() {
    let mut sum: f64 = 0.0;
    let mut sign: f64 = 1.0;
    for i in 0i32..100_000_000 {
        sum += sign / (2.0 * i as f64 + 1.0);
        sign = -sign;
    }
    let pi = sum * 4.0;
    println!("{:.14}", pi);
}
