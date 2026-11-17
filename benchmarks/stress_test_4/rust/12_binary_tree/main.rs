use std::env;
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
