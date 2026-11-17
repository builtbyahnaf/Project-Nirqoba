const { performance } = require('perf_hooks');

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
