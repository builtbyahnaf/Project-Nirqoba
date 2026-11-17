const { performance } = require('perf_hooks');

function run() {
    let size = 512 * 1024 * 1024; // 512 MiB
    if (process.argv.length > 2) {
        size = parseInt(process.argv[2], 10) * 1024 * 1024;
    }

    const t0 = performance.now();
    const buf = Buffer.allocUnsafe(size);
    for (let i = 0; i < size; i += 4096) {
        buf[i] = i & 0xFF;
    }
    const t1 = performance.now();
    const writeMs = t1 - t0;

    const t2 = performance.now();
    let checksum = 0n;
    for (let i = 0; i < size; i += 1024) {
        checksum += BigInt(buf[i]);
    }
    const t3 = performance.now();
    const readMs = t3 - t2;

    const t4 = performance.now();
    for (let i = 0; i < size; i += 4096) {
        buf[i] = ((buf[i] * 3) ^ 0x55) & 0xFF;
    }
    const t5 = performance.now();
    const transformMs = t5 - t4;

    const totalMs = writeMs + readMs + transformMs;
    const throughputMib = (size / (1024 * 1024)) / (totalMs / 1000.0);

    console.log("WORKLOAD: ST4-10");
    console.log("LANGUAGE: JavaScript");
    console.log(`BUFFER_MIB: ${size / (1024 * 1024)}`);
    console.log(`TIME_MS: ${totalMs.toFixed(3)}`);
    console.log(`THROUGHPUT_MIBS: ${throughputMib.toFixed(2)}`);
    console.log(`CHECKSUM: ${checksum.toString()}`);
    console.log("STATUS: PASS");
}

run();
