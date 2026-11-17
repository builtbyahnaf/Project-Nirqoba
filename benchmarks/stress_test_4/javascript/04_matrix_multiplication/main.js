const { performance } = require('perf_hooks');

function run() {
    let size = 1024;
    if (process.argv.length > 2) {
        size = parseInt(process.argv[2], 10);
    }

    const a = new Float64Array(size * size);
    const b = new Float64Array(size * size);
    const c = new Float64Array(size * size);

    for (let i = 0; i < size; i++) {
        for (let j = 0; j < size; j++) {
            a[i * size + j] = ((i + j) % 100) / 100.0;
            b[i * size + j] = ((i * j) % 100) / 100.0;
        }
    }

    const t0 = performance.now();
    // Cache-friendly i-k-j loop
    for (let i = 0; i < size; i++) {
        const iOff = i * size;
        for (let k = 0; k < size; k++) {
            const aIk = a[iOff + k];
            const kOff = k * size;
            for (let j = 0; j < size; j++) {
                c[iOff + j] += aIk * b[kOff + j];
            }
        }
    }

    const t1 = performance.now();
    const timeMs = t1 - t0;

    let checksum = 0.0;
    for (let i = 0; i < size * size; i++) {
        checksum += c[i];
    }

    console.log("WORKLOAD: ST4-04");
    console.log("LANGUAGE: JavaScript");
    console.log(`MATRIX_DIM: ${size}x${size}`);
    console.log(`TIME_MS: ${timeMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${checksum.toFixed(6)}`);
    console.log("STATUS: PASS");
}

run();
