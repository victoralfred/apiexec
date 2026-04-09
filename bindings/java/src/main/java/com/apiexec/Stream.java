package com.apiexec;

import com.sun.jna.*;
import com.sun.jna.ptr.IntByReference;

/**
 * Java binding for the apiexec streaming execution engine.
 * Uses JNA to call the ABI-stable C API.
 */
public class Stream implements AutoCloseable {

    // --- JNA interface ---
    public interface ApiExecLib extends Library {
        ApiExecLib INSTANCE = Native.load("apiexec", ApiExecLib.class);

        Pointer stream_create(String adapter, String config_json, String policy_json);
        void stream_destroy(Pointer handle);
        int stream_has_next(Pointer handle);
        int stream_next_batch_v1(Pointer handle, byte[] buf, int buf_len, IntByReference out_count);
        void stream_cancel(Pointer handle);
    }

    // --- Error codes ---
    public static final int STREAM_OK = 0;
    public static final int STREAM_EXHAUSTED = 1;

    private static final int DEFAULT_BUF_SIZE = 1024 * 1024;

    private Pointer handle;
    private final int bufSize;

    public Stream(String adapter, String configJson) {
        this(adapter, configJson, null, DEFAULT_BUF_SIZE);
    }

    public Stream(String adapter, String configJson, String policyJson) {
        this(adapter, configJson, policyJson, DEFAULT_BUF_SIZE);
    }

    public Stream(String adapter, String configJson, String policyJson, int bufSize) {
        this.bufSize = bufSize;
        this.handle = ApiExecLib.INSTANCE.stream_create(adapter, configJson, policyJson);
        if (this.handle == null) {
            throw new RuntimeException("stream_create failed for adapter: " + adapter);
        }
    }

    public boolean hasNext() {
        if (handle == null) return false;
        return ApiExecLib.INSTANCE.stream_has_next(handle) == 1;
    }

    /**
     * Fetch the next batch as JSON bytes.
     * @return the JSON data and record count, or null if exhausted.
     */
    public BatchResult nextBatch() {
        if (handle == null) throw new IllegalStateException("Stream is closed");

        byte[] buf = new byte[bufSize];
        IntByReference count = new IntByReference(0);
        int rc = ApiExecLib.INSTANCE.stream_next_batch_v1(handle, buf, bufSize, count);

        if (rc == STREAM_EXHAUSTED) return null;
        if (rc != STREAM_OK) throw new RuntimeException("apiexec error: " + rc);

        // Find null terminator
        int len = 0;
        while (len < buf.length && buf[len] != 0) len++;

        byte[] data = new byte[len];
        System.arraycopy(buf, 0, data, 0, len);
        return new BatchResult(data, count.getValue());
    }

    public void cancel() {
        if (handle != null) {
            ApiExecLib.INSTANCE.stream_cancel(handle);
        }
    }

    @Override
    public void close() {
        if (handle != null) {
            ApiExecLib.INSTANCE.stream_destroy(handle);
            handle = null;
        }
    }

    public record BatchResult(byte[] data, int count) {
        public String json() {
            return new String(data);
        }
    }
}
