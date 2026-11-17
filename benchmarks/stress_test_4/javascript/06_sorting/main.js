const { performance } = require('perf_hooks');

function run() {
    let n = 5000000;
    if (process.argv.length > 2) {
        n = parseInt(process.argv[2], 10);
    }

    // Generate deterministic pseudo-random input
    let state = 0x12345678;
    const arr = new Int32Array(n);
    for (let i = 0; i < n; i++) {
        state = Math.imul(state, 1664525) + 1013904223 | 0;
        arr[i] = state;
    }

    const t0 = performance.now();
    arr.sort();
    const t1 = performance.now();
    const sortMs = t1 - t0;

    let isSorted = true;
    for (let i = 0; i < Math.min(n - 1, 10000); i++) {
        if (arr[i] > arr[i + 1]) {
            isSorted = false;
            break;
        }
    }

    let checksum = 0n;
    for (let i = 0; i < n; i += 1000) {
        checksum += BigInt(arr[i]);
    }

    console.log("WORKLOAD: ST4-06");
    console.log("LANGUAGE: JavaScript");
    console.log(`ELEMENTS: ${n}`);
    console.log(`TIME_MS: ${sortMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${checksum.toString()}`);
    console.log(`STATUS: ${isSorted ? "PASS" : "FAIL"}`);
}

run();
