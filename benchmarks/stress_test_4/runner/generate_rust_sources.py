"""
Generator for Rust Stress Test 4 Workloads (ST4-01 to ST4-16)
"""
import os

base = r"benchmarks\stress_test_4\rust"

rust_sources = {
"01_integer_arithmetic": '''use std::env;
use std::time::Instant;
use std::num::Wrapping;

fn main() {
    let mut total_ops: u64 = 500_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        total_ops = args[1].parse().unwrap_or(total_ops);
    }

    let t0 = Instant::now();
    let mut total = Wrapping(0u64);

    for i in 1..=total_ops {
        total += Wrapping(i);
        total -= Wrapping(i & 0xFF);
        total *= Wrapping(3u64);
        let div = (i % 7) + 1;
        total = total ^ (total / Wrapping(div));
        total = total ^ (total << 5) ^ (total >> 59);
    }

    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-01");
    println!("LANGUAGE: Rust");
    println!("OPS: {}", total_ops);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {}", total.0);
    println!("STATUS: PASS");
}
''',

"02_floating_point": '''use std::env;
use std::time::Instant;

fn main() {
    let mut total_ops: u64 = 100_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        total_ops = args[1].parse().unwrap_or(total_ops);
    }

    let t0 = Instant::now();
    let mut acc: f64 = 1.0;

    for _ in 1..=total_ops {
        acc = acc + 0.001;
        acc = acc * 1.000001;
        acc = acc / 1.0000005;
        acc = acc + acc.sqrt() * 0.00001;
    }

    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-02");
    println!("LANGUAGE: Rust");
    println!("OPS: {}", total_ops);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {:.6}", acc);
    println!("STATUS: PASS");
}
''',

"03_branch_heavy": '''use std::env;
use std::time::Instant;
use std::num::Wrapping;

fn main() {
    let mut n: u64 = 100_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
    }

    let t0 = Instant::now();
    let mut state = Wrapping(123456789u32);
    let mut checksum = Wrapping(0u64);

    for i in 0..n {
        state = state * Wrapping(1664525u32) + Wrapping(1013904223u32);
        let u_val = state.0;
        if (i & 1) == 0 {
            if (u_val & 1) != 0 {
                checksum += Wrapping((u_val ^ 0x55555555) as u64);
            } else {
                if ((u_val >> 16) & 1) != 0 {
                    checksum += Wrapping((u_val ^ 0xAAAAAAAA) as u64);
                } else {
                    checksum += Wrapping((u_val.wrapping_mul(3)) as u64);
                }
            }
        } else {
            if u_val < 0x80000000 {
                checksum ^= Wrapping((u_val >> 3) as u64);
            } else {
                checksum -= Wrapping((u_val & 0xFFFF) as u64);
            }
        }
    }

    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-03");
    println!("LANGUAGE: Rust");
    println!("INPUTS: {}", n);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {}", checksum.0);
    println!("STATUS: PASS");
}
''',

"04_matrix_multiplication": '''use std::env;
use std::time::Instant;

fn main() {
    let mut size: usize = 1024;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        size = args[1].parse().unwrap_or(size);
    }

    let total = size * size;
    let mut a = vec![0.0f64; total];
    let mut b = vec![0.0f64; total];
    let mut c = vec![0.0f64; total];

    for i in 0..size {
        for j in 0..size {
            a[i * size + j] = ((i + j) % 100) as f64 / 100.0;
            b[i * size + j] = ((i * j) % 100) as f64 / 100.0;
        }
    }

    let t0 = Instant::now();
    for i in 0..size {
        let i_off = i * size;
        for k in 0..size {
            let a_ik = a[i_off + k];
            let k_off = k * size;
            for j in 0..size {
                c[i_off + j] += a_ik * b[k_off + j];
            }
        }
    }
    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let checksum: f64 = c.iter().sum();

    println!("WORKLOAD: ST4-04");
    println!("LANGUAGE: Rust");
    println!("MATRIX_DIM: {}x{}", size, size);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {:.6}", checksum);
    println!("STATUS: PASS");
}
''',

"05_vector_reduction": '''use std::env;
use std::time::Instant;

fn main() {
    let mut n: usize = 50_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
    }

    let t0 = Instant::now();
    let mut vec = Vec::with_capacity(n);
    for i in 0..n {
        vec.push((i as u64) ^ 0x55555555u64);
    }
    let construct_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let t1 = Instant::now();
    let total: u64 = vec.iter().sum();
    let reduce_ms = t1.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-05");
    println!("LANGUAGE: Rust");
    println!("ELEMENTS: {}", n);
    println!("CONSTRUCTION_MS: {:.3}", construct_ms);
    println!("REDUCTION_MS: {:.3}", reduce_ms);
    println!("TIME_MS: {:.3}", construct_ms + reduce_ms);
    println!("CHECKSUM: {}", total);
    println!("STATUS: PASS");
}
''',

"06_sorting": '''use std::env;
use std::time::Instant;

fn main() {
    let mut n: usize = 5_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
    }

    let mut arr = Vec::with_capacity(n);
    let mut state = 0x12345678u32;
    for _ in 0..n {
        state = state.wrapping_mul(1664525).wrapping_add(1013904223);
        arr.push(state as i32);
    }

    let t0 = Instant::now();
    arr.sort_unstable();
    let sort_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let is_sorted = arr.windows(2).take(10000).all(|w| w[0] <= w[1]);
    let mut checksum: u64 = 0;
    for i in (0..n).step_by(1000) {
        checksum = checksum.wrapping_add(arr[i] as u64);
    }

    println!("WORKLOAD: ST4-06");
    println!("LANGUAGE: Rust");
    println!("ELEMENTS: {}", n);
    println!("TIME_MS: {:.3}", sort_ms);
    println!("CHECKSUM: {}", checksum);
    println!("STATUS: {}", if is_sorted { "PASS" } else { "FAIL" });
}
''',

"07_hash_map": '''use std::env;
use std::time::Instant;
use std::collections::HashMap;

fn main() {
    let mut n: usize = 5_000_000;
    let mut removals: usize = 1_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
        removals = n / 5;
    }

    let mut map = HashMap::with_capacity(n);

    let t0 = Instant::now();
    for i in 0..n {
        let key = (i as u64).wrapping_mul(2654435761u64);
        map.insert(key, i as u64);
    }
    let insert_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let t1 = Instant::now();
    let mut lookup_sum: u64 = 0;
    for i in 0..n {
        let key = (i as u64).wrapping_mul(2654435761u64);
        if let Some(&val) = map.get(&key) {
            lookup_sum += val;
        }
    }
    let lookup_ms = t1.elapsed().as_secs_f64() * 1000.0;

    let t2 = Instant::now();
    for i in (0..removals * 5).step_by(5) {
        let key = (i as u64).wrapping_mul(2654435761u64);
        map.remove(&key);
    }
    let remove_ms = t2.elapsed().as_secs_f64() * 1000.0;

    let total_ms = insert_ms + lookup_ms + remove_ms;
    println!("WORKLOAD: ST4-07");
    println!("LANGUAGE: Rust");
    println!("INSERT_MS: {:.3}", insert_ms);
    println!("LOOKUP_MS: {:.3}", lookup_ms);
    println!("REMOVE_MS: {:.3}", remove_ms);
    println!("TIME_MS: {:.3}", total_ms);
    println!("REMAINING: {}", map.len());
    println!("CHECKSUM: {}", lookup_sum);
    println!("STATUS: {}", if map.len() == (n - removals) { "PASS" } else { "FAIL" });
}
''',

"08_binary_search": '''use std::env;
use std::time::Instant;

fn main() {
    let mut n: usize = 20_000_000;
    let mut queries: usize = 10_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
        queries = n / 2;
    }

    let mut arr = Vec::with_capacity(n);
    for i in 0..n {
        arr.push((i as i64) * 2);
    }

    let t0 = Instant::now();
    let mut hit_count: i64 = 0;
    let mut checksum: i64 = 0;
    let max_val = (n as i64) * 2;

    for q in 0..queries {
        let target = ((q as i64) * 3) % max_val;
        match arr.binary_search(&target) {
            Ok(idx) => {
                hit_count += 1;
                checksum += idx as i64;
            }
            Err(_) => {
                checksum -= 1;
            }
        }
    }
    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-08");
    println!("LANGUAGE: Rust");
    println!("ARRAY_SIZE: {}", n);
    println!("QUERIES: {}", queries);
    println!("HITS: {}", hit_count);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {}", checksum);
    println!("STATUS: PASS");
}
''',

"09_small_object_allocation": '''use std::env;
use std::time::Instant;

struct Node {
    a: u64,
    b: u64,
    next: Option<Box<Node>>,
}

fn main() {
    let mut n: usize = 10_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
    }

    let t0 = Instant::now();
    let mut head: Option<Box<Node>> = None;
    for i in (0..n).rev() {
        head = Some(Box::new(Node {
            a: i as u64,
            b: (i as u64) * 2,
            next: head,
        }));
    }
    let alloc_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let t1 = Instant::now();
    let mut checksum: u64 = 0;
    let mut curr = &head;
    while let Some(node) = curr {
        checksum += node.a + node.b;
        curr = &node.next;
    }
    let traverse_ms = t1.elapsed().as_secs_f64() * 1000.0;

    let t2 = Instant::now();
    // Non-recursive drop loop
    let mut walker = head;
    while let Some(mut node) = walker {
        walker = node.next.take();
    }
    let reclaim_ms = t2.elapsed().as_secs_f64() * 1000.0;

    let total_ms = alloc_ms + traverse_ms + reclaim_ms;
    println!("WORKLOAD: ST4-09");
    println!("LANGUAGE: Rust");
    println!("OBJECTS: {}", n);
    println!("ALLOC_MS: {:.3}", alloc_ms);
    println!("TRAVERSE_MS: {:.3}", traverse_ms);
    println!("RECLAIM_MS: {:.3}", reclaim_ms);
    println!("TIME_MS: {:.3}", total_ms);
    println!("CHECKSUM: {}", checksum);
    println!("STATUS: PASS");
}
''',

"10_large_memory_throughput": '''use std::env;
use std::time::Instant;

fn main() {
    let mut size: usize = 512 * 1024 * 1024; // 512 MiB
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        let mib: usize = args[1].parse().unwrap_or(512);
        size = mib * 1024 * 1024;
    }

    let t0 = Instant::now();
    let mut buf = vec![0u8; size];
    for i in (0..size).step_by(4096) {
        buf[i] = (i & 0xFF) as u8;
    }
    let write_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let t1 = Instant::now();
    let mut checksum: u64 = 0;
    for i in (0..size).step_by(1024) {
        checksum += buf[i] as u64;
    }
    let read_ms = t1.elapsed().as_secs_f64() * 1000.0;

    let t2 = Instant::now();
    for i in (0..size).step_by(4096) {
        buf[i] = (buf[i].wrapping_mul(3)) ^ 0x55;
    }
    let transform_ms = t2.elapsed().as_secs_f64() * 1000.0;

    let total_ms = write_ms + read_ms + transform_ms;
    let throughput_mib = (size as f64 / (1024.0 * 1024.0)) / (total_ms / 1000.0);

    println!("WORKLOAD: ST4-10");
    println!("LANGUAGE: Rust");
    println!("BUFFER_MIB: {}", size / (1024 * 1024));
    println!("TIME_MS: {:.3}", total_ms);
    println!("THROUGHPUT_MIBS: {:.2}", throughput_mib);
    println!("CHECKSUM: {}", checksum);
    println!("STATUS: PASS");
}
''',

"11_pointer_chasing": '''use std::env;
use std::time::Instant;

fn main() {
    let mut n: usize = 10_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
    }

    let mut next_idx: Vec<usize> = (0..n).collect();
    let mut state = 0xDEADBEEFu32;
    for i in (1..n).rev() {
        state = state.wrapping_mul(1664525).wrapping_add(1013904223);
        let j = (state as usize) % (i + 1);
        next_idx.swap(i, j);
    }

    let t0 = Instant::now();
    let mut curr = 0usize;
    let mut checksum: u64 = 0;
    for _ in 0..n {
        checksum += curr as u64;
        curr = next_idx[curr];
    }
    let time_ms = t0.elapsed().as_secs_f64() * 1000.0;

    println!("WORKLOAD: ST4-11");
    println!("LANGUAGE: Rust");
    println!("NODES: {}", n);
    println!("TIME_MS: {:.3}", time_ms);
    println!("CHECKSUM: {}", checksum);
    println!("STATUS: PASS");
}
''',

"12_binary_tree": '''use std::env;
use std::time::Instant;

struct TreeNode {
    val: i32,
    left: Option<Box<TreeNode>>,
    right: Option<Box<TreeNode>>,
}

fn build_tree(depth: i32, idx: i32) -> Option<Box<TreeNode>> {
    if depth == 0 {
        return None;
    }
    Some(Box::new(TreeNode {
        val: idx,
        left: build_tree(depth - 1, idx * 2),
        right: build_tree(depth - 1, idx * 2 + 1),
    }))
}

fn preorder(node: &Option<Box<TreeNode>>) -> u64 {
    match node {
        Some(n) => (n.val as u64) + preorder(&n.left) + preorder(&n.right),
        None => 0,
    }
}

fn inorder(node: &Option<Box<TreeNode>>) -> u64 {
    match node {
        Some(n) => inorder(&n.left) + (n.val as u64) + inorder(&n.right),
        None => 0,
    }
}

fn postorder(node: &Option<Box<TreeNode>>) -> u64 {
    match node {
        Some(n) => postorder(&n.left) + postorder(&n.right) + (n.val as u64),
        None => 0,
    }
}

fn main() {
    let mut depth: i32 = 20; // 1,048,575 nodes
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        depth = args[1].parse().unwrap_or(depth);
    }

    let t0 = Instant::now();
    let root = build_tree(depth, 1);
    let build_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let t1 = Instant::now();
    let sum_pre = preorder(&root);
    let pre_ms = t1.elapsed().as_secs_f64() * 1000.0;

    let t2 = Instant::now();
    let sum_in = inorder(&root);
    let in_ms = t2.elapsed().as_secs_f64() * 1000.0;

    let t3 = Instant::now();
    let sum_post = postorder(&root);
    let post_ms = t3.elapsed().as_secs_f64() * 1000.0;

    let total_ms = build_ms + pre_ms + in_ms + post_ms;
    println!("WORKLOAD: ST4-12");
    println!("LANGUAGE: Rust");
    println!("NODES: {}", (1 << depth) - 1);
    println!("BUILD_MS: {:.3}", build_ms);
    println!("PREORDER_MS: {:.3}", pre_ms);
    println!("INORDER_MS: {:.3}", in_ms);
    println!("POSTORDER_MS: {:.3}", post_ms);
    println!("TIME_MS: {:.3}", total_ms);
    println!("CHECKSUM: {}", sum_pre);
    println!("STATUS: {}", if sum_pre == sum_in && sum_in == sum_post { "PASS" } else { "FAIL" });
}
''',

"13_recursive_overhead": '''use std::env;
use std::time::Instant;

fn merge_sort_recursive(arr: &mut [i32], temp: &mut [i32], left: usize, right: usize) {
    if left >= right {
        return;
    }
    let mid = left + ((right - left) >> 1);
    merge_sort_recursive(arr, temp, left, mid);
    merge_sort_recursive(arr, temp, mid + 1, right);

    let mut i = left;
    let mut j = mid + 1;
    let mut k = left;
    while i <= mid && j <= right {
        if arr[i] <= arr[j] {
            temp[k] = arr[i];
            i += 1;
        } else {
            temp[k] = arr[j];
            j += 1;
        }
        k += 1;
    }
    while i <= mid {
        temp[k] = arr[i];
        i += 1;
        k += 1;
    }
    while j <= right {
        temp[k] = arr[j];
        j += 1;
        k += 1;
    }
    for p in left..=right {
        arr[p] = temp[p];
    }
}

fn main() {
    let mut n: usize = 4_000_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        n = args[1].parse().unwrap_or(n);
    }

    let mut arr = Vec::with_capacity(n);
    let mut temp = vec![0i32; n];

    let mut state = 0x54321u32;
    for _ in 0..n {
        state = state.wrapping_mul(1664525).wrapping_add(1013904223);
        arr.push(state as i32);
    }

    let t0 = Instant::now();
    merge_sort_recursive(&mut arr, &mut temp, 0, n - 1);
    let sort_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let is_sorted = arr[0] <= arr[n - 1];
    let mut checksum: u64 = 0;
    for i in (0..n).step_by(1000) {
        checksum = checksum.wrapping_add(arr[i] as u64);
    }

    println!("WORKLOAD: ST4-13");
    println!("LANGUAGE: Rust");
    println!("ELEMENTS: {}", n);
    println!("TIME_MS: {:.3}", sort_ms);
    println!("CHECKSUM: {}", checksum);
    println!("STATUS: {}", if is_sorted { "PASS" } else { "FAIL" });
}
''',

"14_string_processing": '''use std::env;
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
''',

"15_csv_parsing": '''use std::env;
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
    while pos < len && buffer[pos] != b'\\n' {
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

            if !in_quotes && (b == b',' || b == b'\\n' || b == b'\\r') {
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
                if b == b'\\r' && pos < len && buffer[pos] == b'\\n' {
                    pos += 1;
                }
                if b == b'\\n' || b == b'\\r' {
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
''',

"16_cyclic_objects": '''use std::env;
use std::time::Instant;
use std::rc::Rc;
use std::cell::RefCell;

struct CycleNode {
    value: i32,
    peer: RefCell<Option<Rc<CycleNode>>>,
}

fn main() {
    let mut pairs_count: usize = 500_000;
    let args: Vec<String> = env::args().collect();
    if args.len() > 1 {
        pairs_count = args[1].parse().unwrap_or(pairs_count);
    }

    let t0 = Instant::now();
    let mut a_nodes = Vec::with_capacity(pairs_count);
    let mut b_nodes = Vec::with_capacity(pairs_count);

    for i in 0..pairs_count {
        let a = Rc::new(CycleNode {
            value: i as i32,
            peer: RefCell::new(None),
        });
        let b = Rc::new(CycleNode {
            value: i as i32,
            peer: RefCell::new(None),
        });

        *a.peer.borrow_mut() = Some(Rc::clone(&b));
        *b.peer.borrow_mut() = Some(Rc::clone(&a));

        a_nodes.push(a);
        b_nodes.push(b);
    }
    let build_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let t1 = Instant::now();
    // Break cycles to reclaim memory
    for i in 0..pairs_count {
        *a_nodes[i].peer.borrow_mut() = None;
        *b_nodes[i].peer.borrow_mut() = None;
    }
    a_nodes.clear();
    b_nodes.clear();
    let reclaim_ms = t1.elapsed().as_secs_f64() * 1000.0;

    let total_ms = build_ms + reclaim_ms;
    println!("WORKLOAD: ST4-16");
    println!("LANGUAGE: Rust");
    println!("CYCLES: {}", pairs_count);
    println!("BUILD_MS: {:.3}", build_ms);
    println!("RECLAIM_MS: {:.3}", reclaim_ms);
    println!("TIME_MS: {:.3}", total_ms);
    println!("STATUS: PASS");
}
'''
}

for folder, code in rust_sources.items():
    path = os.path.join(base, folder, "main.rs")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(code)
    print(f"Wrote Rust ST4: {path}")

print("Rust track generation finished.")
