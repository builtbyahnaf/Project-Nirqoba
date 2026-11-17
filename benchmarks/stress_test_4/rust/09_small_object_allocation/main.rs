use std::env;
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
