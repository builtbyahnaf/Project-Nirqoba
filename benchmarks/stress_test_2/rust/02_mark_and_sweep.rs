// Benchmark 02 - Mark and sweep, Rust track.
//
// Same reachable precise binary tree as the C, C++, and Nirqoba tracks. The
// embedded collector is the only allocator used, so allocation, marking, and
// sweeping all exercise the same tracing-GC design.
mod mini_gc;

use mini_gc::{MiniGc, Object};
use std::time::Instant;

const TREE_DEPTH: i32 = 20;
const TREE_NODES: u32 = (1u32 << (TREE_DEPTH + 1)) - 1;
const WARMUP_BATCHES: usize = 3;
const COLLECT_BATCH: usize = 5;
const MEASURE_ROUNDS: usize = 5;

#[repr(C)]
struct Node {
    header: Object,
    left: *mut Node,
    right: *mut Node,
    value: u64,
}

unsafe fn trace_node(object: *mut Object, stack: &mut Vec<*mut Object>) {
    let node = object as *mut Node;
    if !(*node).left.is_null() {
        stack.push((*node).left as *mut Object);
    }
    if !(*node).right.is_null() {
        stack.push((*node).right as *mut Object);
    }
}

fn alloc_node(gc: &mut MiniGc, value: u64) -> *mut Node {
    let object = gc.alloc(std::mem::size_of::<Node>(), Some(trace_node));
    let node = object as *mut Node;
    unsafe {
        (*node).value = value;
    }
    node
}

fn build_tree(gc: &mut MiniGc, depth: i32, value: u64) -> *mut Node {
    let node = alloc_node(gc, value);
    if depth > 0 {
        unsafe {
            (*node).left = build_tree(gc, depth - 1, value * 2);
            (*node).right = build_tree(gc, depth - 1, value * 2 + 1);
        }
    }
    node
}

fn sum_tree(node: *mut Node, depth: i32) -> u64 {
    unsafe {
        if depth == 0 {
            return (*node).value;
        }
        (*node).value
            + sum_tree((*node).left, depth - 1)
            + sum_tree((*node).right, depth - 1)
    }
}

fn now_ms() -> f64 {
    static START: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
    START.get_or_init(Instant::now).elapsed().as_secs_f64() * 1000.0
}

fn main() {
    let mut gc = MiniGc::new(64 * 1024 * 1024);

    let build_start = now_ms();
    let root = build_tree(&mut gc, TREE_DEPTH, 1);
    let build_ms = now_ms() - build_start;
    gc.set_root(root as *mut Object);

    let traverse_start = now_ms();
    let checksum = sum_tree(root, TREE_DEPTH);
    let traverse_ms = now_ms() - traverse_start;

    for _ in 0..WARMUP_BATCHES {
        for _ in 0..COLLECT_BATCH {
            gc.collect();
        }
    }

    println!("TEST mark_and_sweep");
    println!("IMPL rust");
    println!("OBJECTS {}", TREE_NODES);
    println!("WARMUP_BATCHES {}", WARMUP_BATCHES);
    println!("COLLECT_BATCH {}", COLLECT_BATCH);
    println!("MEASURE_ROUNDS {}", MEASURE_ROUNDS);
    println!("BUILD_MS {:.3}", build_ms);
    println!("TRAVERSE_MS {:.3}", traverse_ms);
    println!("CHECKSUM {}", checksum);

    for round in 0..MEASURE_ROUNDS {
        let before = gc.collections();
        let start = now_ms();
        for _ in 0..COLLECT_BATCH {
            gc.collect();
        }
        let elapsed = now_ms() - start;
        println!("ROUND {}", round);
        println!("COLLECT_MS {:.3}", elapsed / COLLECT_BATCH as f64);
        println!("LIVE_AFTER {}", gc.live_objects());
        println!("COLLECTIONS_DELTA {}", gc.collections() - before);
    }
}
