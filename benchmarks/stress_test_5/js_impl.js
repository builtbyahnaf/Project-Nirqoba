// Stress Test 5 - the CDMM gauntlet, Node.js (V8) baseline.
//
// V8 allocates a young generation for the short-lived objects, the tree is a
// typed array block, and the dropped graph is an Array that is cleared before an
// explicit collect (run with --expose-gc).

'use strict';

const STACK_ITERATIONS = 200000;
const POOL_ITERATIONS = 2000;
const POOL_SLOTS = 8192;
const TREE_NODES = 8191;
const GC_NODES = 40000;

function stackPhase() {
  let checksum = 0;
  for (let i = 0; i < STACK_ITERATIONS; i++) {
    const v = { x: i };
    checksum += v.x;
  }
  return checksum;
}

function poolPhase() {
  let events = 0;
  for (let i = 0; i < POOL_ITERATIONS; i++) {
    const batch = new Array(POOL_SLOTS);
    batch[0] = { kind: i, payload: i * 2 };
    events += batch[0].kind + batch[0].payload;
  }
  return events;
}

function arenaPhase() {
  const tree = new Int32Array(TREE_NODES * 3);
  tree[0] = 5;
  tree[1] = 1;
  tree[2] = 2;
  let treeSum = tree[0] + tree[1] + tree[2];

  const leaf = { x: 4 };
  treeSum += leaf.x;
  return treeSum;
}

function gcPhase() {
  const kept = new Array(GC_NODES);
  let cycles = 0;
  for (let i = 0; i < GC_NODES; i++) {
    const node = { value: i, next: null };
    kept[i] = node;
    cycles += node.value;
  }
  kept.length = 0;
  if (typeof global.gc === 'function') {
    global.gc();
  }
  return cycles;
}

console.log(stackPhase());
console.log(poolPhase());
console.log(arenaPhase());
console.log(gcPhase());
