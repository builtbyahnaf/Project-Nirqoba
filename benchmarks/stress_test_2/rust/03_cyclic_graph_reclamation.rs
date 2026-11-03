// Benchmark 03 - Cyclic graph reclamation, Rust track.
//
// Two-node cycles with no external root, traced and swept by the embedded
// precise mark-sweep collector. Construction is timed separately from
// reclamation.
mod mini_gc;

use mini_gc::{MiniGc, Object};
use std::time::Instant;

const CYCLE_PAIRS: usize = 500_000;
const CYCLE_NODES: usize = CYCLE_PAIRS * 2;
const WARMUP_ROUNDS: usize = 2;
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

fn build_cycles(gc: &mut MiniGc, pairs: usize) -> u64 {
    let mut checksum = 0u64;
    for index in 0..pairs {
        let first = alloc_node(gc, (index * 2) as u64);
        let second = alloc_node(gc, (index * 2 + 1) as u64);
        unsafe {
            (*first).left = second;
            (*second).left = first;
            checksum += (*first).value + (*second).value;
        }
    }
    checksum
}

fn now_ms() -> f64 {
    static START: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
    START.get_or_init(Instant::now).elapsed().as_secs_f64() * 1000.0
}

fn main() {
    let mut gc = MiniGc::new(64 * 1024 * 1024);
    gc.set_root(std::ptr::null_mut());

    let mut checksum = 0u64;
    for _ in 0..WARMUP_ROUNDS {
        checksum = build_cycles(&mut gc, CYCLE_PAIRS);
        gc.collect();
    }

    println!("TEST cyclic_graph_reclamation");
    println!("IMPL rust");
    println!("OBJECTS {}", CYCLE_NODES);
    println!("WARMUP_ROUNDS {}", WARMUP_ROUNDS);
    println!("MEASURE_ROUNDS {}", MEASURE_ROUNDS);

    for round in 0..MEASURE_ROUNDS {
        let build_start = now_ms();
        let round_checksum = build_cycles(&mut gc, CYCLE_PAIRS);
        let build_ms = now_ms() - build_start;
        checksum = round_checksum;

        let collect_start = now_ms();
        let live = gc.collect();
        let collect_ms = now_ms() - collect_start;

        println!("ROUND {}", round);
        println!("BUILD_MS {:.3}", build_ms);
        println!("COLLECT_MS {:.3}", collect_ms);
        println!("CHECKSUM {}", round_checksum);
        println!("LIVE_AFTER {}", live);
    }

    println!("FINAL_CHECKSUM {}", checksum);
}
