const { performance } = require('perf_hooks');

class Node {
    constructor(a, b, next) {
        this.a = a;
        this.b = b;
        this.next = next;
    }
}

function run() {
    let n = 10000000;
    if (process.argv.length > 2) {
        n = parseInt(process.argv[2], 10);
    }

    // Allocation & link
    const t0 = performance.now();
    let head = null;
    let curr = null;
    for (let i = 0; i < n; i++) {
        const node = new Node(i, i * 2, null);
        if (head === null) {
            head = node;
            curr = node;
        } else {
            curr.next = node;
            curr = node;
        }
    }
    const t1 = performance.now();
    const allocMs = t1 - t0;

    // Traversal & touch
    const t2 = performance.now();
    let checksum = 0n;
    let walker = head;
    while (walker !== null) {
        checksum += BigInt(walker.a + walker.b);
        walker = walker.next;
    }
    const t3 = performance.now();
    const traverseMs = t3 - t2;

    // Reclamation
    const t4 = performance.now();
    head = null;
    curr = null;
    walker = null;
    if (global.gc) {
        global.gc();
    }
    const t5 = performance.now();
    const reclaimMs = t5 - t4;

    const totalMs = allocMs + traverseMs + reclaimMs;
    console.log("WORKLOAD: ST4-09");
    console.log("LANGUAGE: JavaScript");
    console.log(`OBJECTS: ${n}`);
    console.log(`ALLOC_MS: ${allocMs.toFixed(3)}`);
    console.log(`TRAVERSE_MS: ${traverseMs.toFixed(3)}`);
    console.log(`RECLAIM_MS: ${reclaimMs.toFixed(3)}`);
    console.log(`TIME_MS: ${totalMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${checksum.toString()}`);
    console.log("STATUS: PASS");
}

run();
