fn main() {
    let n = 512usize;
    let size = n * n;
    let mut a: Vec<f64> = vec![0.0; size];
    let mut b: Vec<f64> = vec![0.0; size];
    let mut c: Vec<f64> = vec![0.0; size];

    for i in 0..n {
        for j in 0..n {
            a[i*n+j] = (i + j) as f64 * 0.001;
            b[i*n+j] = (i as i64 - j as i64) as f64 * 0.001;
        }
    }
    for r in 0..n {
        for k in 0..n {
            let aik = a[r*n+k];
            for col in 0..n {
                c[r*n+col] += aik * b[k*n+col];
            }
        }
    }
    let check = c[0] + c[n*256+256] + c[n*511+511];
    println!("{:.6}", check);
}
