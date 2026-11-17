const { performance } = require('perf_hooks');

function merge(left, right) {
    const result = new Int32Array(left.length + right.length);
    let i = 0, j = 0, k = 0;
    while (i < left.length && j < right.length) {
        if (left[i] <= right[j]) {
            result[k++] = left[i++];
        } else {
            result[k++] = right[j++];
        }
    }
    while (i < left.length) result[k++] = left[i++];
    while (j < right.length) result[k++] = right[j++];
    return result;
}

function mergeSort(arr) {
    if (arr.length <= 1) return arr;
    const mid = arr.length >> 1;
    const left = mergeSort(arr.subarray(0, mid));
    const right = mergeSort(arr.subarray(mid));
    return merge(left, right);
}

function run() {
    let n = 4000000;
    if (process.argv.length > 2) {
        n = parseInt(process.argv[2], 10);
    }

    let state = 0x54321;
    const arr = new Int32Array(n);
    for (let i = 0; i < n; i++) {
        state = Math.imul(state, 1664525) + 1013904223 | 0;
        arr[i] = state;
    }

    const t0 = performance.now();
    const sortedArr = mergeSort(arr);
    const t1 = performance.now();
    const sortMs = t1 - t0;

    const isSorted = sortedArr[0] <= sortedArr[sortedArr.length - 1];
    let checksum = 0n;
    for (let i = 0; i < sortedArr.length; i += 1000) {
        checksum += BigInt(sortedArr[i]);
    }

    console.log("WORKLOAD: ST4-13");
    console.log("LANGUAGE: JavaScript");
    console.log(`ELEMENTS: ${n}`);
    console.log(`TIME_MS: ${sortMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${checksum.toString()}`);
    console.log(`STATUS: ${isSorted ? "PASS" : "FAIL"}`);
}

run();
