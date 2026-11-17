const { performance } = require('perf_hooks');

function run() {
    let n = 50000000;
    if (process.argv.length > 2) {
        n = parseInt(process.argv[2], 10);
    }

    const t0 = performance.now();
    // Construction phase
    const vec = new BigUint64Array(n);
    for (let i = 0; i < n; i++) {
        vec[i] = BigInt(i) ^ 0x55555555n;
    }
    const t1 = performance.now();
    const constructMs = t1 - t0;

    // Reduction phase
    const t2 = performance.now();
    let total = 0n;
    for (let i = 0; i < n; i++) {
        total += vec[i];
    }
    const t3 = performance.now();
    const reduceMs = t3 - t2;

    console.log("WORKLOAD: ST4-05");
    console.log("LANGUAGE: JavaScript");
    console.log(`ELEMENTS: ${n}`);
    console.log(`CONSTRUCTION_MS: ${constructMs.toFixed(3)}`);
    console.log(`REDUCTION_MS: ${reduceMs.toFixed(3)}`);
    console.log(`TIME_MS: ${(constructMs + reduceMs).toFixed(3)}`);
    console.log(`CHECKSUM: ${total.toString()}`);
    console.log("STATUS: PASS");
}

run();
