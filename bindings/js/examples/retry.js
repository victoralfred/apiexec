// Example: retry and rate-limit adaptation.
//
// Run: LD_LIBRARY_PATH=../../build node examples/retry.js

'use strict';

const http = require('http');

let Stream;
try {
    ({ Stream } = require('../src/index'));
} catch (e) {
    console.log('SKIP: ffi-napi not installed —', e.message);
    process.exit(0);
}

let requestCount = 0;

const server = http.createServer((req, res) => {
    requestCount++;
    if (requestCount <= 3) {
        res.writeHead(429, {
            'Content-Type': 'application/json',
            'Retry-After': '0',
        });
        res.end('{"error":"rate limited"}');
        return;
    }

    const url = new URL(req.url, 'http://localhost');
    const page = parseInt(url.searchParams.get('cursor') || '0');
    const body = {
        data: [{ id: page, value: `record_${page}` }],
        next: page < 2 ? String(page + 1) : null,
    };
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(body));
});

server.listen(0, '127.0.0.1', () => {
    const port = server.address().port;
    const config = JSON.stringify({ base_url: `http://127.0.0.1:${port}` });
    const policy = '{"max_retries":5,"base_backoff_ms":10,"prefetch_depth":0}';

    const stream = new Stream('generic_rest', config, policy);
    let pages = 0;
    let records = 0;

    try {
        while (stream.hasNext()) {
            const result = stream.nextBatch();
            if (!result) break;
            pages++;
            records += result.count;
            console.log(`Page ${pages}: ${result.count} records (${result.json.length} bytes)`);
        }
    } finally {
        stream.close();
    }

    console.log(`\nResult: ${pages} pages, ${records} records`);
    console.log(`Mock server served ${requestCount} requests (3 x 429 + 3 successes)`);
    server.close();
});
