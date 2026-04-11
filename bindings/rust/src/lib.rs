//! Safe Rust bindings for the apiexec streaming execution engine.
//!
//! Wraps the ABI-stable C API. The [`Stream`] type manages the handle
//! lifecycle and provides an iterator-like interface.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::ptr;

// --- FFI declarations ---

#[repr(C)]
pub struct StreamHandle {
    _private: [u8; 0],
}

extern "C" {
    fn stream_create(
        adapter: *const c_char,
        config_json: *const c_char,
        policy_json: *const c_char,
    ) -> *mut StreamHandle;
    fn stream_destroy(handle: *mut StreamHandle);
    fn stream_has_next(handle: *mut StreamHandle) -> i32;
    fn stream_next_batch_v1(
        handle: *mut StreamHandle,
        buf: *mut c_void,
        buf_len: i32,
        out_count: *mut i32,
    ) -> i32;
    fn stream_cancel(handle: *mut StreamHandle);
}

const STREAM_OK: i32 = 0;
const STREAM_EXHAUSTED: i32 = 1;

// --- Error type ---

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    CreateFailed,
    RateLimit,
    Server,
    Client,
    Parse,
    Network,
    Cancelled,
    InvalidArg,
    BudgetExhausted,
    Unknown(i32),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::CreateFailed => write!(f, "stream creation failed"),
            Error::RateLimit => write!(f, "rate limited"),
            Error::Server => write!(f, "server error"),
            Error::Client => write!(f, "client error"),
            Error::Parse => write!(f, "parse error"),
            Error::Network => write!(f, "network error"),
            Error::Cancelled => write!(f, "cancelled"),
            Error::InvalidArg => write!(f, "invalid argument"),
            Error::BudgetExhausted => write!(f, "budget exhausted"),
            Error::Unknown(code) => write!(f, "unknown error code {code}"),
        }
    }
}

impl std::error::Error for Error {}

fn error_from_code(code: i32) -> Error {
    match code {
        -1 => Error::RateLimit,
        -2 => Error::Server,
        -3 => Error::Client,
        -4 => Error::Parse,
        -5 => Error::Network,
        -6 => Error::Cancelled,
        -7 => Error::InvalidArg,
        -8 => Error::BudgetExhausted,
        _ => Error::Unknown(code),
    }
}

// --- Stream ---

/// A streaming connection to an API via apiexec.
///
/// Owns a C `StreamHandle`. Destroyed on drop.
pub struct Stream {
    handle: *mut StreamHandle,
}

// SAFETY: stream_cancel is thread-safe per the C API contract. Callers must
// not call next_batch/has_next concurrently from multiple threads without
// external synchronisation — this is documented in the public API.
unsafe impl Send for Stream {}
unsafe impl Sync for Stream {}

impl Stream {
    /// Create a new stream. `policy_json` may be empty for defaults.
    pub fn new(adapter: &str, config_json: &str, policy_json: &str) -> Result<Self, Error> {
        let c_adapter = CString::new(adapter).map_err(|_| Error::InvalidArg)?;
        let c_config = CString::new(config_json).map_err(|_| Error::InvalidArg)?;

        let c_policy = if policy_json.is_empty() {
            ptr::null()
        } else {
            CString::new(policy_json).map_err(|_| Error::InvalidArg)?.into_raw()
        };

        // SAFETY: All pointers are valid C strings or null.
        let handle = unsafe {
            stream_create(c_adapter.as_ptr(), c_config.as_ptr(), c_policy)
        };

        // Free the policy string if we allocated it
        if !c_policy.is_null() {
            // SAFETY: We created this with into_raw above.
            unsafe { drop(CString::from_raw(c_policy as *mut c_char)); }
        }

        if handle.is_null() {
            return Err(Error::CreateFailed);
        }

        Ok(Stream { handle })
    }

    /// Returns true if more data is available.
    pub fn has_next(&self) -> bool {
        // SAFETY: handle is valid (non-null, owned by self).
        unsafe { stream_has_next(self.handle) == 1 }
    }

    /// Fetch the next batch as raw JSON bytes and record count.
    pub fn next_batch(&self, buf_size: usize) -> Result<(Vec<u8>, i32), Error> {
        let buf_size = if buf_size == 0 { 1024 * 1024 } else { buf_size };
        let mut buf = vec![0u8; buf_size];
        let mut count: i32 = 0;

        // SAFETY: buf is a valid allocation of buf_size bytes.
        let rc = unsafe {
            stream_next_batch_v1(
                self.handle,
                buf.as_mut_ptr() as *mut c_void,
                buf_size as i32,
                &mut count,
            )
        };

        if rc == STREAM_OK {
            // Find null terminator
            let len = buf.iter().position(|&b| b == 0).unwrap_or(buf_size);
            buf.truncate(len);
            Ok((buf, count))
        } else if rc == STREAM_EXHAUSTED {
            Err(Error::Cancelled) // use Cancelled to signal exhaustion in iteration
        } else {
            Err(error_from_code(rc))
        }
    }

    /// Cancel the stream. Safe to call from any thread.
    pub fn cancel(&self) {
        // SAFETY: stream_cancel is documented as thread-safe.
        unsafe { stream_cancel(self.handle); }
    }

    /// Iterate all batches, calling `f` for each JSON payload.
    pub fn for_each<F>(&self, mut f: F) -> Result<(), Error>
    where
        F: FnMut(&[u8], i32) -> bool,
    {
        while self.has_next() {
            match self.next_batch(0) {
                Ok((data, count)) => {
                    if !f(&data, count) {
                        return Ok(());
                    }
                }
                Err(Error::Cancelled) => return Ok(()), // exhausted
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }
}

impl Drop for Stream {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            // SAFETY: handle is valid and owned by self.
            unsafe { stream_destroy(self.handle); }
            self.handle = ptr::null_mut();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_unknown_adapter() {
        let result = Stream::new("nonexistent", r#"{"base_url":"http://x"}"#, "");
        assert!(matches!(result, Err(Error::CreateFailed)));
    }

    #[test]
    fn test_invalid_json() {
        let result = Stream::new("generic_rest", "not json", "");
        assert!(matches!(result, Err(Error::CreateFailed)));
    }

    #[test]
    fn test_create_valid() {
        let stream = Stream::new(
            "generic_rest",
            r#"{"base_url":"http://localhost:9999"}"#,
            r#"{"prefetch_depth": 0}"#,
        )
        .unwrap();
        assert!(stream.has_next());
    }

    #[test]
    fn test_cancel() {
        let stream = Stream::new(
            "generic_rest",
            r#"{"base_url":"http://localhost:9999"}"#,
            r#"{"prefetch_depth": 0}"#,
        )
        .unwrap();
        stream.cancel();
        assert!(!stream.has_next());
    }

    #[test]
    fn test_network_error() {
        let stream = Stream::new(
            "generic_rest",
            r#"{"base_url":"http://127.0.0.1:1"}"#,
            r#"{"max_retries": 0, "prefetch_depth": 0}"#,
        )
        .unwrap();
        let result = stream.next_batch(0);
        assert!(matches!(result, Err(Error::Network)));
    }
}
