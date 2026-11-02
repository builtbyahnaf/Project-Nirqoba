fn main() {
    let mut count = 0i32;
    for _ in 0..1_000_000 {
        let _ptr: Box<[u8; 64]> = Box::new([0u8; 64]);
        count += 1;
        // _ptr dropped here
    }
    println!("{}", count);
}
