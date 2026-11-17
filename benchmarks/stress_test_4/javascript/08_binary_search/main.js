const { performance } = require('perf_hooks');

function binarySearch(arr, target) {
    let low = 0;
    let high = arr.length - 1;
    while (low <= high) {
        const mid = (low + high) >>> 1;
        const midVal = arr[mid];
        if (midVal === target) {
            return mid;
        } else if (midVal < target) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return -1;
}

function run() {
    let n = 20000000;
    let queries = 10000000;
    if (process.argv.length > 2) {
        n = parseInt(process.argv[2], 10);
        queries = Math.floor(n / 2);
    }

    const arr = new Int32Array(n);
    for (let i = 0; i < n; i++) {
        arr[i] = i * 2;
    }

    const t0 = performance.now();
    let hitCount = 0;
    let checksum = 0n;
    const maxVal = n * 2;

    for (let q = 0; q < queries; q++) {
        const target = (q * 3) % maxVal;
        const idx = binarySearch(arr, target);
        if (idx !== -1) {
            hitCount++;
            checksum += BigInt(idx);
        } else {
            checksum -= 1n;
        }
    }

    const t1 = performance.now();
    const timeMs = t1 - t0;

    console.log("WORKLOAD: ST4-08");
    console.log("LANGUAGE: JavaScript");
    console.log(`ARRAY_SIZE: ${n}`);
    console.log(`QUERIES: ${queries}`);
    console.log(`HITS: ${hitCount}`);
    console.log(`TIME_MS: ${timeMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${checksum.toString()}`);
    console.log("STATUS: PASS");
}

run();
