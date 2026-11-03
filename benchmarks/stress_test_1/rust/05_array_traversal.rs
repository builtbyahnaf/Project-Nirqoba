fn main() {
    let size = 10_000_000usize;
    let mut arr: Vec<i64> = (0..size as i64).collect();
    for _ in 0..5 {
        for j in 0..size {
            arr[j] = (arr[j] * 3 + 7) ^ 1431655765i64;
        }
    }
    let sum: i64 = arr.iter().sum();
    println!("{}", sum);
}
