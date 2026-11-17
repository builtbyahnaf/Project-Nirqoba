const { performance } = require('perf_hooks');

class CycleNode {
    constructor(value) {
        this.value = value;
        this.peer = null;
    }
}

function run() {
    let pairsCount = 500000;
    if (process.argv.length > 2) {
        pairsCount = parseInt(process.argv[2], 10);
    }

    const t0 = performance.now();
    let nodes = new Array(pairsCount);
    for (let i = 0; i < pairsCount; i++) {
        const a = new CycleNode(i);
        const b = new CycleNode(i);
        a.peer = b;
        b.peer = a;
        nodes[i] = a;
    }
    const t1 = performance.now();
    const buildMs = t1 - t0;

    // Drop external roots
    nodes = null;

    const t2 = performance.now();
    if (global.gc) {
        global.gc();
    }
    const t3 = performance.now();
    const reclaimMs = t3 - t2;

    const totalMs = buildMs + reclaimMs;
    console.log("WORKLOAD: ST4-16");
    console.log("LANGUAGE: JavaScript");
    console.log(`CYCLES: ${pairsCount}`);
    console.log(`BUILD_MS: ${buildMs.toFixed(3)}`);
    console.log(`RECLAIM_MS: ${reclaimMs.toFixed(3)}`);
    console.log(`TIME_MS: ${totalMs.toFixed(3)}`);
    console.log("STATUS: PASS");
}

run();
