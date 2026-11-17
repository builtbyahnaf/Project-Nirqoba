const { performance } = require('perf_hooks');

class TreeNode {
    constructor(val, left = null, right = null) {
        this.val = val;
        this.left = left;
        this.right = right;
    }
}

function buildTree(depth, idx = 1) {
    if (depth === 0) return null;
    return new TreeNode(idx, buildTree(depth - 1, idx * 2), buildTree(depth - 1, idx * 2 + 1));
}

function preorder(node) {
    if (!node) return 0n;
    return BigInt(node.val) + preorder(node.left) + preorder(node.right);
}

function inorder(node) {
    if (!node) return 0n;
    return inorder(node.left) + BigInt(node.val) + inorder(node.right);
}

function postorder(node) {
    if (!node) return 0n;
    return postorder(node.left) + postorder(node.right) + BigInt(node.val);
}

function run() {
    let depth = 20; // 1,048,575 nodes
    if (process.argv.length > 2) {
        depth = parseInt(process.argv[2], 10);
    }

    const t0 = performance.now();
    const root = buildTree(depth);
    const t1 = performance.now();
    const buildMs = t1 - t0;

    const t2 = performance.now();
    const sumPre = preorder(root);
    const t3 = performance.now();
    const preMs = t3 - t2;

    const t4 = performance.now();
    const sumIn = inorder(root);
    const t5 = performance.now();
    const inMs = t5 - t4;

    const t6 = performance.now();
    const sumPost = postorder(root);
    const t7 = performance.now();
    const postMs = t7 - t6;

    const totalMs = buildMs + preMs + inMs + postMs;
    console.log("WORKLOAD: ST4-12");
    console.log("LANGUAGE: JavaScript");
    console.log(`NODES: ${(1 << depth) - 1}`);
    console.log(`BUILD_MS: ${buildMs.toFixed(3)}`);
    console.log(`PREORDER_MS: ${preMs.toFixed(3)}`);
    console.log(`INORDER_MS: ${inMs.toFixed(3)}`);
    console.log(`POSTORDER_MS: ${postMs.toFixed(3)}`);
    console.log(`TIME_MS: ${totalMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${sumPre.toString()}`);
    console.log(`STATUS: ${sumPre === sumIn && sumIn === sumPost ? "PASS" : "FAIL"}`);
}

run();
