fn consume_array(arr: Vec<i64>) -> i64 {
    arr.iter().sum()
    // arr dropped here
}

fn main() {
    let mut total: i64 = 0;
    for i in 0i64..1_000_000 {
        let arr: Vec<i64> = (0..8).map(|j| i + j).collect();
        total += consume_array(arr);
    }
    println!("{}", total);
}
