/**
 * Node.js bindings for the apiexec streaming execution engine.
 * Uses node-ffi-napi to call the ABI-stable C API.
 *
 * Note: Requires `ffi-napi` and `ref-napi` packages.
 * If those are not available, this module exports a stub with
 * documentation of the intended API.
 */

'use strict';

let ffi, ref;
try {
    ffi = require('ffi-napi');
    ref = require('ref-napi');
} catch {
    // ffi-napi not installed — export documented stubs
    module.exports = {
        Stream: class Stream {
            constructor() {
                throw new Error(
                    'apiexec: ffi-napi not installed. Run: npm install ffi-napi ref-napi'
                );
            }
        },
        STREAM_OK: 0,
        STREAM_EXHAUSTED: 1,
    };
    return;
}

const path = require('path');

const STREAM_OK = 0;
const STREAM_EXHAUSTED = 1;
const DEFAULT_BUF_SIZE = 1024 * 1024;

// Locate shared library
const libPath = path.resolve(__dirname, '../../../build/libapiexec.so');

const lib = ffi.Library(libPath, {
    stream_create: ['pointer', ['string', 'string', 'string']],
    stream_destroy: ['void', ['pointer']],
    stream_has_next: ['int32', ['pointer']],
    stream_next_batch_v1: ['int32', ['pointer', 'pointer', 'int32', ref.refType('int32')]],
    stream_cancel: ['void', ['pointer']],
});

class Stream {
    /**
     * @param {string} adapter - Adapter name (e.g., "generic_rest")
     * @param {string} configJson - JSON configuration string
     * @param {string} [policyJson] - JSON policy string (optional)
     */
    constructor(adapter, configJson, policyJson = null) {
        this._handle = lib.stream_create(adapter, configJson, policyJson);
        if (this._handle.isNull()) {
            throw new Error(`stream_create failed for adapter: ${adapter}`);
        }
        this._bufSize = DEFAULT_BUF_SIZE;
    }

    hasNext() {
        if (!this._handle) return false;
        return lib.stream_has_next(this._handle) === 1;
    }

    /**
     * Fetch the next batch as a JSON string and record count.
     * @returns {{ json: string, count: number } | null} null if exhausted
     */
    nextBatch() {
        if (!this._handle) throw new Error('Stream is closed');

        const buf = Buffer.alloc(this._bufSize);
        const countRef = ref.alloc('int32', 0);
        const rc = lib.stream_next_batch_v1(this._handle, buf, this._bufSize, countRef);

        if (rc === STREAM_EXHAUSTED) return null;
        if (rc !== STREAM_OK) throw new Error(`apiexec error: ${rc}`);

        const nullIdx = buf.indexOf(0);
        const json = buf.toString('utf8', 0, nullIdx >= 0 ? nullIdx : this._bufSize);
        return { json, count: countRef.deref() };
    }

    cancel() {
        if (this._handle) lib.stream_cancel(this._handle);
    }

    close() {
        if (this._handle) {
            lib.stream_destroy(this._handle);
            this._handle = null;
        }
    }
}

module.exports = { Stream, STREAM_OK, STREAM_EXHAUSTED };
