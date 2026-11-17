const { performance } = require('perf_hooks');

function run() {
    let n = 100000000;
    if (process.argv.length > 2) {
        n = parseInt(process.argv[2], 10);
    }

    const t0 = performance.now();
    let state = 123456789;
    let checksum = 0n;

    for (let i = 0; i < n; i++) {
        state = Math.imul(state, 1664525) + 1013904223 | 0;
        const uState = state >>> 0;
        if ((i & 1) === 0) {
            if ((uState & 1) !== 0) {
                checksum += BigInt(uState ^ 0x55555555);
            } else {
                if (((uState >>> 16) & 1) !== 0) {
                    checksum += BigInt(uState ^ 0xAAAAAAAA);
                } else {
                    checksum += BigInt(Math.imul(uState, 3) >>> 0);
                }
            }
        } else {
            if (uState < 0x80000000) {
                checksum ^= BigInt(uState >>> 3);
            } else {
                checksum -= BigInt(uState & 0xFFFF);
            }
        }
    }

    const t1 = performance.now();
    const timeMs = t1 - t0;

    console.log("WORKLOAD: ST4-03");
    console.log("LANGUAGE: JavaScript");
    console.log(`INPUTS: ${n}`);
    console.log(`TIME_MS: ${timeMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${checksum.toString()}`);
    console.log("STATUS: PASS");
}

run();
