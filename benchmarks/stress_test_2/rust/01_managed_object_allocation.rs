// Benchmark 01 - Managed-object allocation, Rust track.
//
// Native Box<Node> chain baseline. Rust's ownership model frees the chain
// deterministically; this is the no-tracing-GC baseline beside Nirqoba's managed
// heap and Java's collector. Release is iterative so a one-million-node chain
// does not recurse through Drop.
use std::hint::black_box;
use std::time::Instant;

const OBJECT_COUNT: usize = 1_000_000;
const WARMUP_ROUNDS: usize = 2;
const MEASURE_ROUNDS: usize = 5;

struct Node {
    next: Option<Box<Node>>,
    value: u64,
}

fn build_chain(count: usize) -> Option<Box<Node>> {
    let mut head = Box::new(Node {
        next: None,
        value: 0,
    });
    let mut tail: &mut Node = &mut head;
    for index in 1..count {
        tail.next = Some(Box::new(Node {
            next: None,
            value: index as u64,
        }));
        tail = tail.next.as_deref_mut().unwrap();
    }
    Some(head)
}

fn sum_chain(head: &Option<Box<Node>>, count: usize) -> u64 {
    let mut total = 0u64;
    let mut cursor = head.as_deref();
    for _ in 0..count {
        let node = cursor.unwrap();
        total += node.value;
        cursor = node.next.as_deref();
    }
    total
}

fn free_chain(head: Option<Box<Node>>) {
    let mut current = head;
    while let Some(mut node) = current {
        current = node.next.take();
    }
}

fn now_ms() -> f64 {
    static START: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
    START.get_or_init(Instant::now).elapsed().as_secs_f64() * 1000.0
}

fn run_round(verbose: bool) -> u64 {
    let alloc_start = now_ms();
    let head = build_chain(OBJECT_COUNT);
    let alloc_ms = now_ms() - alloc_start;

    let traverse_start = now_ms();
    let checksum = sum_chain(&head, OBJECT_COUNT);
    let traverse_ms = now_ms() - traverse_start;

    let collect_start = now_ms();
    free_chain(head);
    let collect_ms = now_ms() - collect_start;

    if verbose {
        println!("ALLOC_MS {:.3}", alloc_ms);
        println!("TRAVERSE_MS {:.3}", traverse_ms);
        println!("COLLECT_MS {:.3}", collect_ms);
        println!("CHECKSUM {}", black_box(checksum));
        println!("LIVE_AFTER 0");
    }
    checksum
}

fn main() {
    for _ in 0..WARMUP_ROUNDS {
        black_box(run_round(false));
    }

    println!("TEST managed_object_allocation");
    println!("IMPL rust");
    println!("OBJECTS {}", OBJECT_COUNT);
    println!("WARMUP_ROUNDS {}", WARMUP_ROUNDS);
    println!("MEASURE_ROUNDS {}", MEASURE_ROUNDS);

    for round in 0..MEASURE_ROUNDS {
        println!("ROUND {}", round);
        black_box(run_round(true));
    }
}
