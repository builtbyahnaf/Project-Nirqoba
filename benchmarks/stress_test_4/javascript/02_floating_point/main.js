const { performance } = require('perf_hooks');

function run() {
    let totalOps = 100000000;
    if (process.argv.length > 2) {
        totalOps = parseInt(process.argv[2], 10);
    }

    const t0 = performance.now();
    let acc = 1.0;

    for (let i = 1; i <= totalOps; i++) {
        acc = acc + 0.001;
        acc = acc * 1.000001;
        acc = acc / 1.0000005;
        acc = acc + Math.sqrt(acc) * 0.00001;
    }

    const t1 = performance.now();
    const timeMs = t1 - t0;

    console.log("WORKLOAD: ST4-02");
    console.log("LANGUAGE: JavaScript");
    console.log(`OPS: ${totalOps}`);
    console.log(`TIME_MS: ${timeMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${acc.toFixed(6)}`);
    console.log("STATUS: PASS");
}

run();
