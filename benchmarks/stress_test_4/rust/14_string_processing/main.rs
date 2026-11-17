use std::env;
use std::time::Instant;

fn main() {
    let mut size_mb: usize = 256;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        size_mb = args[1].parse().unwrap_or(size_mb);
    }

    let total_bytes = size_mb * 1024 * 1024;
    let pattern = "Nirqoba high performance compiler benchmark suite text processing tokenization. ";
    let repeats = total_bytes / pattern.len() + 1;
    let mut corpus = String::with_capacity(total_bytes);
    for _ in 0..repeats {
        corpus.push_str(pattern);
        if corpus.len() >= total_bytes {
            break;
        }
    }
    corpus.truncate(total_bytes);

    // 1. Character count
    let t0 = Instant::now();
    let char_count = corpus.bytes().filter(|&b| b == b'e').count() as u64;
    let char_ms = t0.elapsed().as_secs_f64() * 1000.0;

    // 2. Substring search
    let t1 = Instant::now();
    let sub_count = corpus.match_indices("benchmark").count() as u64;
    let sub_ms = t1.elapsed().as_secs_f64() * 1000.0;

    // 3. Whitespace tokenization
    let t2 = Instant::now();
    let token_count = corpus.split_whitespace().count() as u64;
    let tok_ms = t2.elapsed().as_secs_f64() * 1000.0;

    // 4. Case conversion
    let t3 = Instant::now();
    let upper_corpus = corpus.to_ascii_uppercase();
    let case_ms = t3.elapsed().as_secs_f64() * 1000.0;

    let total_ms = char_ms + sub_ms + tok_ms + case_ms;
    println!("WORKLOAD: ST4-14");
    println!("LANGUAGE: Rust");
    println!("CORPUS_MIB: {}", size_mb);
    println!("CHAR_COUNT_MS: {:.3}", char_ms);
    println!("SUBSTRING_MS: {:.3}", sub_ms);
    println!("TOKENIZE_MS: {:.3}", tok_ms);
    println!("CASE_CONV_MS: {:.3}", case_ms);
    println!("TIME_MS: {:.3}", total_ms);
    println!("CHECKSUM: {}", char_count + sub_count + token_count);
    println!("STATUS: PASS");
    let _ = upper_corpus.len();
}
