use std::env;
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
