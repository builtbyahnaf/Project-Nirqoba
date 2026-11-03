fn main() {
    let size = 10_000_000usize;
    let mut buf: Vec<u8> = Vec::with_capacity(size);

    for i in 0..size {
        let modv = i % 40;
        if modv == 39 { buf.push(32u8); }
        else { buf.push(((modv % 26) + 97) as u8); }
    }

    let mut words: i64 = 0;
    let mut vowels: i64 = 0;
    for &b in &buf {
        if b == 32 { words += 1; }
        else if b == b'a' || b == b'e' || b == b'i' || b == b'o' || b == b'u' { vowels += 1; }
    }

    for ch in buf.iter_mut() {
        if *ch >= 97 && *ch <= 122 {
            *ch = ((*ch - 97 + 13) % 26 + 97) as u8;
        }
    }

    // Match Nirqoba order: hash = (hash ^ byte) * prime
    let mut hash: u64 = 14695981039346656037u64;
    for &b in &buf {
        hash = (hash ^ b as u64).wrapping_mul(1099511628211u64);
    }

    println!("{}", words);
    println!("{}", vowels);
    println!("{}", hash as i64);
}
