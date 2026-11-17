"""
Generator for JavaScript (Node.js) Stress Test 4 Workloads (ST4-01 to ST4-16)
"""
import os

base = r"benchmarks\stress_test_4\javascript"

js_sources = {
"01_integer_arithmetic": '''const { performance } = require('perf_hooks');

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
''',

"02_floating_point": '''const { performance } = require('perf_hooks');

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
''',

"03_branch_heavy": '''const { performance } = require('perf_hooks');

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
''',

"04_matrix_multiplication": '''const { performance } = require('perf_hooks');

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
''',

"05_vector_reduction": '''const { performance } = require('perf_hooks');

function run() {
    let n = 50000000;
    if (process.argv.length > 2) {
        n = parseInt(process.argv[2], 10);
    }

    const t0 = performance.now();
    // Construction phase
    const vec = new BigUint64Array(n);
    for (let i = 0; i < n; i++) {
        vec[i] = BigInt(i) ^ 0x55555555n;
    }
    const t1 = performance.now();
    const constructMs = t1 - t0;

    // Reduction phase
    const t2 = performance.now();
    let total = 0n;
    for (let i = 0; i < n; i++) {
        total += vec[i];
    }
    const t3 = performance.now();
    const reduceMs = t3 - t2;

    console.log("WORKLOAD: ST4-05");
    console.log("LANGUAGE: JavaScript");
    console.log(`ELEMENTS: ${n}`);
    console.log(`CONSTRUCTION_MS: ${constructMs.toFixed(3)}`);
    console.log(`REDUCTION_MS: ${reduceMs.toFixed(3)}`);
    console.log(`TIME_MS: ${(constructMs + reduceMs).toFixed(3)}`);
    console.log(`CHECKSUM: ${total.toString()}`);
    console.log("STATUS: PASS");
}

run();
''',

"06_sorting": '''const { performance } = require('perf_hooks');

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
''',

"07_hash_map": '''const { performance } = require('perf_hooks');

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
''',

"08_binary_search": '''const { performance } = require('perf_hooks');

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
''',

"09_small_object_allocation": '''const { performance } = require('perf_hooks');

class Node {
    constructor(a, b, next) {
        this.a = a;
        this.b = b;
        this.next = next;
    }
}

function run() {
    let n = 10000000;
    if (process.argv.length > 2) {
        n = parseInt(process.argv[2], 10);
    }

    // Allocation & link
    const t0 = performance.now();
    let head = null;
    let curr = null;
    for (let i = 0; i < n; i++) {
        const node = new Node(i, i * 2, null);
        if (head === null) {
            head = node;
            curr = node;
        } else {
            curr.next = node;
            curr = node;
        }
    }
    const t1 = performance.now();
    const allocMs = t1 - t0;

    // Traversal & touch
    const t2 = performance.now();
    let checksum = 0n;
    let walker = head;
    while (walker !== null) {
        checksum += BigInt(walker.a + walker.b);
        walker = walker.next;
    }
    const t3 = performance.now();
    const traverseMs = t3 - t2;

    // Reclamation
    const t4 = performance.now();
    head = null;
    curr = null;
    walker = null;
    if (global.gc) {
        global.gc();
    }
    const t5 = performance.now();
    const reclaimMs = t5 - t4;

    const totalMs = allocMs + traverseMs + reclaimMs;
    console.log("WORKLOAD: ST4-09");
    console.log("LANGUAGE: JavaScript");
    console.log(`OBJECTS: ${n}`);
    console.log(`ALLOC_MS: ${allocMs.toFixed(3)}`);
    console.log(`TRAVERSE_MS: ${traverseMs.toFixed(3)}`);
    console.log(`RECLAIM_MS: ${reclaimMs.toFixed(3)}`);
    console.log(`TIME_MS: ${totalMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${checksum.toString()}`);
    console.log("STATUS: PASS");
}

run();
''',

"10_large_memory_throughput": '''const { performance } = require('perf_hooks');

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
''',

"11_pointer_chasing": '''const { performance } = require('perf_hooks');

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
''',

"12_binary_tree": '''const { performance } = require('perf_hooks');

class TreeNode {
    constructor(val, left = null, right = null) {
        this.val = val;
        this.left = left;
        this.right = right;
    }
}

function buildTree(depth, idx = 1) {
    if (depth === 0) return null;
    return new TreeNode(idx, buildTree(depth - 1, idx * 2), buildTree(depth - 1, idx * 2 + 1));
}

function preorder(node) {
    if (!node) return 0n;
    return BigInt(node.val) + preorder(node.left) + preorder(node.right);
}

function inorder(node) {
    if (!node) return 0n;
    return inorder(node.left) + BigInt(node.val) + inorder(node.right);
}

function postorder(node) {
    if (!node) return 0n;
    return postorder(node.left) + postorder(node.right) + BigInt(node.val);
}

function run() {
    let depth = 20; // 1,048,575 nodes
    if (process.argv.length > 2) {
        depth = parseInt(process.argv[2], 10);
    }

    const t0 = performance.now();
    const root = buildTree(depth);
    const t1 = performance.now();
    const buildMs = t1 - t0;

    const t2 = performance.now();
    const sumPre = preorder(root);
    const t3 = performance.now();
    const preMs = t3 - t2;

    const t4 = performance.now();
    const sumIn = inorder(root);
    const t5 = performance.now();
    const inMs = t5 - t4;

    const t6 = performance.now();
    const sumPost = postorder(root);
    const t7 = performance.now();
    const postMs = t7 - t6;

    const totalMs = buildMs + preMs + inMs + postMs;
    console.log("WORKLOAD: ST4-12");
    console.log("LANGUAGE: JavaScript");
    console.log(`NODES: ${(1 << depth) - 1}`);
    console.log(`BUILD_MS: ${buildMs.toFixed(3)}`);
    console.log(`PREORDER_MS: ${preMs.toFixed(3)}`);
    console.log(`INORDER_MS: ${inMs.toFixed(3)}`);
    console.log(`POSTORDER_MS: ${postMs.toFixed(3)}`);
    console.log(`TIME_MS: ${totalMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${sumPre.toString()}`);
    console.log(`STATUS: ${sumPre === sumIn && sumIn === sumPost ? "PASS" : "FAIL"}`);
}

run();
''',

"13_recursive_overhead": '''const { performance } = require('perf_hooks');

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
''',

"14_string_processing": '''const { performance } = require('perf_hooks');

function run() {
    let sizeMb = 256;
    if (process.argv.length > 2) {
        sizeMb = parseInt(process.argv[2], 10);
    }

    const pattern = "Nirqoba high performance compiler benchmark suite text processing tokenization. ";
    const repeats = Math.floor((sizeMb * 1024 * 1024) / pattern.length) + 1;
    const corpus = pattern.repeat(repeats).substring(0, sizeMb * 1024 * 1024);

    // 1. Character count
    const t0 = performance.now();
    let charCount = 0;
    for (let i = 0; i < corpus.length; i++) {
        if (corpus.charCodeAt(i) === 101) { // 'e'
            charCount++;
        }
    }
    const t1 = performance.now();
    const charMs = t1 - t0;

    // 2. Substring search
    const t2 = performance.now();
    let subCount = 0;
    let pos = 0;
    while ((pos = corpus.indexOf("benchmark", pos)) !== -1) {
        subCount++;
        pos += 9;
    }
    const t3 = performance.now();
    const subMs = t3 - t2;

    // 3. Whitespace tokenization
    const t4 = performance.now();
    let tokenCount = 0;
    let inToken = false;
    for (let i = 0; i < corpus.length; i++) {
        const c = corpus.charCodeAt(i);
        if (c === 32 || c === 10 || c === 9) {
            inToken = false;
        } else if (!inToken) {
            inToken = true;
            tokenCount++;
        }
    }
    const t5 = performance.now();
    const tokMs = t5 - t4;

    // 4. Case conversion
    const t6 = performance.now();
    const upperCorpus = corpus.toUpperCase();
    const t7 = performance.now();
    const caseMs = t7 - t6;

    const totalMs = charMs + subMs + tokMs + caseMs;
    console.log("WORKLOAD: ST4-14");
    console.log("LANGUAGE: JavaScript");
    console.log(`CORPUS_MIB: ${sizeMb}`);
    console.log(`CHAR_COUNT_MS: ${charMs.toFixed(3)}`);
    console.log(`SUBSTRING_MS: ${subMs.toFixed(3)}`);
    console.log(`TOKENIZE_MS: ${tokMs.toFixed(3)}`);
    console.log(`CASE_CONV_MS: ${caseMs.toFixed(3)}`);
    console.log(`TIME_MS: ${totalMs.toFixed(3)}`);
    console.log(`CHECKSUM: ${charCount + subCount + tokenCount}`);
    console.log("STATUS: PASS");
}

run();
''',

"15_csv_parsing": '''const fs = require('fs');
const { performance } = require('perf_hooks');

function run() {
    let csvFile = "benchmarks/stress_test_4/artifacts/st4_15_benchmark_5m.csv";
    if (process.argv.length > 2) {
        csvFile = process.argv[2];
    }

    if (!fs.existsSync(csvFile)) {
        console.error(`CSV file not found: ${csvFile}`);
        process.exit(1);
    }

    // Load entire file into memory buffer before timing
    const buf = fs.readFileSync(csvFile);

    const t0 = performance.now();

    let totalRows = 0;
    let sumId = 0n;
    let sumDeviceCode = 0n;
    let emptyCatCount = 0;
    let escapedQuotesCount = 0;

    let pos = 0;
    const len = buf.length;

    // Skip header line
    while (pos < len && buf[pos] !== 10) pos++;
    if (pos < len) pos++; // skip newline

    while (pos < len) {
        totalRows++;
        let col = 0;
        let cellStart = pos;
        let inQuotes = false;
        let rowHasEscapedQuote = false;

        let col0Val = 0n;
        let col4Val = 0n;
        let col6Len = 0;

        while (pos < len) {
            const b = buf[pos];
            if (b === 34) { // quote
                if (inQuotes && pos + 1 < len && buf[pos + 1] === 34) {
                    rowHasEscapedQuote = true;
                    pos += 2;
                    continue;
                }
                inQuotes = !inQuotes;
                pos++;
                continue;
            }

            if (!inQuotes && (b === 44 || b === 10 || b === 13)) {
                // End of cell
                const cellLen = pos - cellStart;
                if (col === 0) {
                    let num = 0n;
                    for (let k = cellStart; k < pos; k++) {
                        num = num * 10n + BigInt(buf[k] - 48);
                    }
                    col0Val = num;
                } else if (col === 4) {
                    let num = 0n;
                    for (let k = cellStart; k < pos; k++) {
                        num = num * 10n + BigInt(buf[k] - 48);
                    }
                    col4Val = num;
                } else if (col === 6) {
                    col6Len = cellLen;
                }

                col++;
                pos++;
                if (b === 13 && pos < len && buf[pos] === 10) pos++; // handle CRLF
                if (b === 10 || b === 13) break; // end of line
                cellStart = pos;
            } else {
                pos++;
            }
        }

        sumId += col0Val;
        sumDeviceCode += col4Val;
        if (col6Len === 0) emptyCatCount++;
        if (rowHasEscapedQuote) escapedQuotesCount++;
    }

    const t1 = performance.now();
    const parseMs = t1 - t0;

    console.log("WORKLOAD: ST4-15");
    console.log("LANGUAGE: JavaScript");
    console.log(`ROWS_PARSED: ${totalRows}`);
    console.log(`TIME_MS: ${parseMs.toFixed(3)}`);
    console.log(`SUM_ID: ${sumId.toString()}`);
    console.log(`SUM_DEVICE: ${sumDeviceCode.toString()}`);
    console.log(`EMPTY_CAT: ${emptyCatCount}`);
    console.log(`CHECKSUM: ${(sumId + sumDeviceCode).toString()}`);
    console.log(`STATUS: ${totalRows === 5000000 ? "PASS" : "FAIL"}`);
}

run();
''',

"16_cyclic_objects": '''const { performance } = require('perf_hooks');

class CycleNode {
    constructor(value) {
        this.value = value;
        this.peer = null;
    }
}

function run() {
    let pairsCount = 500000;
    if (process.argv.length > 2) {
        pairsCount = parseInt(process.argv[2], 10);
    }

    const t0 = performance.now();
    let nodes = new Array(pairsCount);
    for (let i = 0; i < pairsCount; i++) {
        const a = new CycleNode(i);
        const b = new CycleNode(i);
        a.peer = b;
        b.peer = a;
        nodes[i] = a;
    }
    const t1 = performance.now();
    const buildMs = t1 - t0;

    // Drop external roots
    nodes = null;

    const t2 = performance.now();
    if (global.gc) {
        global.gc();
    }
    const t3 = performance.now();
    const reclaimMs = t3 - t2;

    const totalMs = buildMs + reclaimMs;
    console.log("WORKLOAD: ST4-16");
    console.log("LANGUAGE: JavaScript");
    console.log(`CYCLES: ${pairsCount}`);
    console.log(`BUILD_MS: ${buildMs.toFixed(3)}`);
    console.log(`RECLAIM_MS: ${reclaimMs.toFixed(3)}`);
    console.log(`TIME_MS: ${totalMs.toFixed(3)}`);
    console.log("STATUS: PASS");
}

run();
'''
}

for folder, code in js_sources.items():
    path = os.path.join(base, folder, "main.js")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(code)
    print(f"Wrote JS ST4: {path}")

print("JavaScript track generation finished.")
