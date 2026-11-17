const { performance } = require('perf_hooks');

function run() {
    let n = 10000000;
    if (process.argv.length > 2) {
        n = parseInt(process.argv[2], 10);
    }

    const nextIdx = new Int32Array(n);
    for (let i = 0; i < n; i++) {
        nextIdx[i] = i;
    }

    let state = 0xDEADBEEF;
    for (let i = n - 1; i > 0; i--) {
        state = Math.imul(state, 1664525) + 1013904223 | 0;
        const uState = state >>> 0;
        const j = uState % (i + 1);
        const tmp = nextIdx[i];
        nextIdx[i] = nextIdx[j];
        nextIdx[j] = tmp;
    }

    const t0 = performance.now();
    let curr = 0;
    let checksum = 0n;
    for (let i = 0; i < n; i++) {
        checksum += BigInt(curr);
        curr = nextIdx[curr];
    }
    const t1 = performance.now();
    const timeMs = t1 - t0;

    console.log("WORKLOAD: ST4-11");
    console.log("LANGUAGE: JavaScript");
    console.log(`NODES: ${n}`);
    console.log(`TIME_MS: ${timeMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${checksum.toString()}`);
    console.log("STATUS: PASS");
}

run();
