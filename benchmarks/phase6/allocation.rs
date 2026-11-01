use std::{env, hint::black_box, time::Instant, thread, sync::atomic::{AtomicU64, Ordering}};
struct Node { value:u64, next:usize }
fn main() {
    let args:Vec<String>=env::args().collect();
    let n:usize=args.get(1).and_then(|s|s.parse().ok()).unwrap_or(1_000_000);
    let mode=args.get(2).map(String::as_str).unwrap_or("alloc");
    let start=Instant::now(); let checksum:u64;
    match mode {
      "alloc" => { let mut sum=0u64; for i in 0..n { let mut p=Box::new([0u64;8]);p[0]=i as u64;p[7]=(i as u64)^0x5a5a;sum=sum.wrapping_add(p[0]+p[7]);black_box(p); } checksum=sum; },
      "scalar" => { let mut x=1u64;for i in 0..n{x=x.wrapping_mul(1664525).wrapping_add(1013904223)^((i as u64)+0x9e37);}checksum=x; },
      "array" => { let a:Vec<u64>=(0..n).map(|i|i as u64*3+1).collect();checksum=a.iter().copied().sum();black_box(a); },
      "graph" => { let count=n.min(500_000);let mut nodes:Vec<Box<Node>>=(0..count).map(|i|Box::new(Node{value:i as u64,next:0})).collect();for i in 0..count{nodes[i].next=(i+1)%count;}let mut at=0usize;let mut sum=0u64;for _ in 0..count.saturating_mul(4){sum=sum.wrapping_add(nodes[at].value);at=nodes[at].next;}checksum=sum; },
      "threads" => { let sum=AtomicU64::new(0);thread::scope(|s|{for t in 0..4{s.spawn(move ||{let mut local=0u64;for i in (t..n).step_by(4){let mut p=Box::new([0u64;8]);p[0]=i as u64;p[7]=(i as u64)^0x5a5a;local=local.wrapping_add(p[0]+p[7]);}sum.fetch_add(local,Ordering::Relaxed);});}});checksum=sum.load(Ordering::Relaxed); },
      _ => std::process::exit(2),
    }
    println!("mode={mode} ns={} checksum={checksum}",start.elapsed().as_nanos());
}

