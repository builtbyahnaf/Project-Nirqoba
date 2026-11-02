fn main() {
    let mut state: i64 = 123456789;
    for i in 0i64..100_000_000 {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state = state.wrapping_add(i);
    }
    println!("{}", state);
}
