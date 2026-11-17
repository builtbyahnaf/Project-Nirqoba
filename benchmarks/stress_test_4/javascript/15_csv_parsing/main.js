const fs = require('fs');
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
