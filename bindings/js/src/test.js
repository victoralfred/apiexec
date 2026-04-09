/**
 * Smoke tests for apiexec Node.js bindings.
 * Run: node src/test.js
 */

'use strict';

let Stream;
try {
    ({ Stream } = require('./index'));
} catch (e) {
    console.log('SKIP: ffi-napi not installed —', e.message);
    process.exit(0);
}

let passed = 0;
let failed = 0;

function assert(cond, msg) {
    if (!cond) {
        console.error(`  FAIL: ${msg}`);
        failed++;
    } else {
        console.log(`  PASS: ${msg}`);
        passed++;
    }
}

// Test: unknown adapter
try {
    new Stream('nonexistent', '{"base_url":"http://x"}');
    assert(false, 'unknown_adapter should throw');
} catch {
    assert(true, 'unknown_adapter');
}

// Test: create valid
try {
    const s = new Stream('generic_rest', '{"base_url":"http://localhost:9999"}',
        '{"prefetch_depth": 0}');
    assert(s.hasNext(), 'create_valid');
    s.close();
} catch (e) {
    assert(false, `create_valid: ${e.message}`);
}

// Test: cancel
try {
    const s = new Stream('generic_rest', '{"base_url":"http://localhost:9999"}',
        '{"prefetch_depth": 0}');
    s.cancel();
    assert(!s.hasNext(), 'cancel');
    s.close();
} catch (e) {
    assert(false, `cancel: ${e.message}`);
}

// Test: close idempotent
try {
    const s = new Stream('generic_rest', '{"base_url":"http://localhost:9999"}');
    s.close();
    s.close();
    assert(true, 'close_idempotent');
} catch (e) {
    assert(false, `close_idempotent: ${e.message}`);
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
