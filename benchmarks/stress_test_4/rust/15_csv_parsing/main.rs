use std::env;
use std::time::Instant;
use std::fs;

fn main() {
    let mut filename = "benchmarks/stress_test_4/artifacts/st4_15_benchmark_5m.csv".to_string();
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        filename = args[1].clone();
    }

    let buffer = fs::read(&filename).expect("Cannot read CSV file");

    let t0 = Instant::now();

    let mut total_rows: u64 = 0;
    let mut sum_id: u64 = 0;
    let mut sum_device_code: u64 = 0;
    let mut empty_cat_count: u64 = 0;
    let mut escaped_quotes_count: u64 = 0;

    let len = buffer.len();
    let mut pos = 0;

    // Skip header
    while pos < len && buffer[pos] != b'\n' {
        pos += 1;
    }
    if pos < len {
        pos += 1;
    }

    while pos < len {
        total_rows += 1;
        let mut col = 0;
        let mut cell_start = pos;
        let mut in_quotes = false;
        let mut row_has_escaped_quote = false;

        let mut col0_val = 0u64;
        let mut col4_val = 0u64;
        let mut col6_len = 0usize;

        while pos < len {
            let b = buffer[pos];
            if b == b'"' {
                if in_quotes && pos + 1 < len && buffer[pos + 1] == b'"' {
                    row_has_escaped_quote = true;
                    pos += 2;
                    continue;
                }
                in_quotes = !in_quotes;
                pos += 1;
                continue;
            }

            if !in_quotes && (b == b',' || b == b'\n' || b == b'\r') {
                let cell_len = pos - cell_start;
                if col == 0 {
                    let mut num = 0u64;
                    for k in cell_start..pos {
                        num = num * 10 + (buffer[k] - b'0') as u64;
                    }
                    col0_val = num;
                } else if col == 4 {
                    let mut num = 0u64;
                    for k in cell_start..pos {
                        num = num * 10 + (buffer[k] - b'0') as u64;
                    }
                    col4_val = num;
                } else if col == 6 {
                    col6_len = cell_len;
                }

                col += 1;
                pos += 1;
                if b == b'\r' && pos < len && buffer[pos] == b'\n' {
                    pos += 1;
                }
                if b == b'\n' || b == b'\r' {
                    break;
                }
                cell_start = pos;
            } else {
                pos += 1;
            }
        }

        sum_id += col0_val;
        sum_device_code += col4_val;
        if col6_len == 0 {
            empty_cat_count += 1;
        }
        if row_has_escaped_quote {
            escaped_quotes_count += 1;
        }
    }

    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-15");
    println!("LANGUAGE: Rust");
    println!("ROWS_PARSED: {}", total_rows);
    println!("TIME_MS: {:.3}", time_ms);
    println!("SUM_ID: {}", sum_id);
    println!("SUM_DEVICE: {}", sum_device_code);
    println!("EMPTY_CAT: {}", empty_cat_count);
    println!("CHECKSUM: {}", sum_id + sum_device_code);
    println!("STATUS: {}", if total_rows == 5000000 { "PASS" } else { "FAIL" });
}
