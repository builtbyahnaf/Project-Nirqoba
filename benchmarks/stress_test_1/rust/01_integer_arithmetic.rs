fn main() {
    let mut total: i64 = 0;
    for i in 0i64..500_000_000 {
        total = ((total + i) * 3) ^ (total / 7);
    }
    println!("{}", total);
}
