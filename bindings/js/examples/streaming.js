// Example: streaming batches with prefetch.
//
// Run: LD_LIBRARY_PATH=../../build node examples/streaming.js

'use strict';

const http = require('http');

let Stream;
try {
    ({ Stream } = require('../src/index'));
} catch (e) {
    console.log('SKIP: ffi-napi not installed —', e.message);
    process.exit(0);
}

const TOTAL_PAGES = 10;
const RECORDS_PER_PAGE = 5;

const server = http.createServer((req, res) => {
    // Simulate 20ms API latency
    setTimeout(() => {
        const url = new URL(req.url, 'http://localhost');
        const page = parseInt(url.searchParams.get('cursor') || '0');

        const data = [];
        for (let i = 0; i < RECORDS_PER_PAGE; i++) {
            data.push({ id: page * RECORDS_PER_PAGE + i });
        }
        const body = {
            data,
            next: page + 1 < TOTAL_PAGES ? String(page + 1) : null,
        };
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(body));
    }, 20);
});

server.listen(0, '127.0.0.1', () => {
    const port = server.address().port;
    const config = JSON.stringify({ base_url: `http://127.0.0.1:${port}` });
    const policy = '{"prefetch_depth":1}'; // enable prefetch

    const stream = new Stream('generic_rest', config, policy);
    const start = Date.now();
    let batches = 0;
    let total = 0;

    try {
        while (stream.hasNext()) {
            const result = stream.nextBatch();
            if (!result) break;
            batches++;
            total += result.count;
            // Simulate 20ms processing — overlapped with background prefetch
            const end = Date.now() + 20;
            while (Date.now() < end) { /* busy wait */ }
            console.log(`Batch ${batches}: ${result.count} records`);
        }
    } finally {
        stream.close();
    }

    const elapsed = Date.now() - start;
    console.log(`\nProcessed ${batches} batches, ${total} records in ${elapsed}ms`);
    console.log('With prefetch, ~max(fetch, process) x N, not fetch + process.');
    server.close();
});
