const { performance } = require('perf_hooks');

function run() {
    let n = 5000000;
    let removals = 1000000;
    if (process.argv.length > 2) {
        n = parseInt(process.argv[2], 10);
        removals = Math.floor(n / 5);
    }

    const map = new Map();

    // Insert phase
    const t0 = performance.now();
    for (let i = 0; i < n; i++) {
        const key = BigInt(i) * 2654435761n & 0xFFFFFFFFFFFFFFFFn;
        map.set(key, i);
    }
    const t1 = performance.now();
    const insertMs = t1 - t0;

    // Lookup phase
    const t2 = performance.now();
    let lookupSum = 0n;
    for (let i = 0; i < n; i++) {
        const key = BigInt(i) * 2654435761n & 0xFFFFFFFFFFFFFFFFn;
        lookupSum += BigInt(map.get(key));
    }
    const t3 = performance.now();
    const lookupMs = t3 - t2;

    // Removal phase
    const t4 = performance.now();
    for (let i = 0; i < removals * 5; i += 5) {
        const key = BigInt(i) * 2654435761n & 0xFFFFFFFFFFFFFFFFn;
        map.delete(key);
    }
    const t5 = performance.now();
    const removeMs = t5 - t4;

    const totalMs = insertMs + lookupMs + removeMs;

    console.log("WORKLOAD: ST4-07");
    console.log("LANGUAGE: JavaScript");
    console.log(`INSERT_MS: ${insertMs.toFixed(3)}`);
    console.log(`LOOKUP_MS: ${lookupMs.toFixed(3)}`);
    console.log(`REMOVE_MS: ${removeMs.toFixed(3)}`);
    console.log(`TIME_MS: ${totalMs.toFixed(3)}`);
    console.log(`REMAINING: ${map.size}`);
    console.log(`CHECKSUM: ${lookupSum.toString()}`);
    console.log(`STATUS: ${map.size === (n - removals) ? "PASS" : "FAIL"}`);
}

run();
