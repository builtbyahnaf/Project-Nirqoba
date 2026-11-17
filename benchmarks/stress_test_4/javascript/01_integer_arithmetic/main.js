const { performance } = require('perf_hooks');

function run() {
    let totalOps = 500000000;
    if (process.argv.length > 2) {
        totalOps = parseInt(process.argv[2], 10);
    }

    const t0 = performance.now();
    let total = 0n;
    const mask64 = 0xFFFFFFFFFFFFFFFFn;

    for (let i = 1n; i <= BigInt(totalOps); i++) {
        total = (total + i) & mask64;
        total = (total - (i & 0xFFn)) & mask64;
        total = (total * 3n) & mask64;
        const div = (i % 7n) + 1n;
        total = (total ^ (total / div)) & mask64;
        total = (total ^ ((total << 5n) & mask64) ^ (total >> 59n)) & mask64;
    }

    const t1 = performance.now();
    const timeMs = t1 - t0;

    console.log("WORKLOAD: ST4-01");
    console.log("LANGUAGE: JavaScript");
    console.log(`OPS: ${totalOps}`);
    console.log(`TIME_MS: ${timeMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${total.toString()}`);
    console.log("STATUS: PASS");
}

run();
