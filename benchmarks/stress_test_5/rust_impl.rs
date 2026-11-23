// Stress Test 5 - the CDMM gauntlet, Rust baseline.
//
// Box for the short-lived values, a Vec<Box<..>> for the dropped graph, one
// block for the tree. Sized to match the tiers the Nirqoba compiler chose.

use std::cell::RefCell;
use std::rc::Rc;

struct Vec1 {
    x: i64,
}

struct Event {
    kind: i64,
    payload: i64,
}

struct CycleNode {
    value: i64,
    next: Option<Rc<RefCell<CycleNode>>>,
}

const STACK_ITERATIONS: usize = 200_000;
const POOL_ITERATIONS: usize = 2_000;
const POOL_SLOTS: usize = 8_192;
const TREE_NODES: usize = 8_191;
const GC_NODES: usize = 40_000;

fn stack_phase() -> i64 {
    let mut checksum = 0i64;
    for i in 0..STACK_ITERATIONS {
        let v = Box::new(Vec1 { x: i as i64 });
        checksum += v.x;
    }
    checksum
}

fn pool_phase() -> i64 {
    let mut events = 0i64;
    for i in 0..POOL_ITERATIONS {
        let mut batch: Vec<Event> = Vec::with_capacity(POOL_SLOTS);
        batch.push(Event {
            kind: i as i64,
            payload: (i * 2) as i64,
        });
        events += batch[0].kind + batch[0].payload;
    }
    events
}

fn arena_phase() -> i64 {
    let mut tree = vec![0i64; TREE_NODES * 3];
    tree[0] = 5;
    tree[1] = 1;
    tree[2] = 2;
    let mut tree_sum = tree[0] + tree[1] + tree[2];

    let leaf = Box::new(Vec1 { x: 4 });
    tree_sum += leaf.x;
    tree_sum
}

fn gc_phase() -> i64 {
    let mut kept: Vec<Rc<RefCell<CycleNode>>> = Vec::with_capacity(GC_NODES);
    let mut cycles = 0i64;
    for i in 0..GC_NODES {
        let node = Rc::new(RefCell::new(CycleNode {
            value: i as i64,
            next: None,
        }));
        cycles += node.borrow().value;
        kept.push(node);
    }
    kept.clear();
    cycles
}

fn main() {
    println!("{}", stack_phase());
    println!("{}", pool_phase());
    println!("{}", arena_phase());
    println!("{}", gc_phase());
}
