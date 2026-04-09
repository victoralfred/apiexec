"""Python bindings for the apiexec streaming execution engine.

Wraps the ABI-stable C API via ctypes. The Stream class manages the
handle lifecycle and provides an iterator interface.
"""

import ctypes
import os
from pathlib import Path
from typing import Callable, Optional, Tuple

# Locate the shared library
_LIB_SEARCH_PATHS = [
    Path(__file__).parent / "../../build/libapiexec.so",
    Path(__file__).parent / "../../build/libapiexec.so.1",
]

_lib = None
for p in _LIB_SEARCH_PATHS:
    if p.exists():
        _lib = ctypes.CDLL(str(p.resolve()))
        break

if _lib is None:
    raise ImportError("Cannot find libapiexec.so — build the project first")

# --- C API function signatures ---

_lib.stream_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
_lib.stream_create.restype = ctypes.c_void_p

_lib.stream_destroy.argtypes = [ctypes.c_void_p]
_lib.stream_destroy.restype = None

_lib.stream_has_next.argtypes = [ctypes.c_void_p]
_lib.stream_has_next.restype = ctypes.c_int32

_lib.stream_next_batch_v1.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32,
    ctypes.POINTER(ctypes.c_int32),
]
_lib.stream_next_batch_v1.restype = ctypes.c_int32

_lib.stream_cancel.argtypes = [ctypes.c_void_p]
_lib.stream_cancel.restype = None

# --- Error codes ---

STREAM_OK = 0
STREAM_EXHAUSTED = 1


class ApiExecError(Exception):
    """Error from the apiexec engine."""

    ERROR_NAMES = {
        -1: "RateLimit",
        -2: "Server",
        -3: "Client",
        -4: "Parse",
        -5: "Network",
        -6: "Cancelled",
        -7: "InvalidArg",
        -8: "BudgetExhausted",
    }

    def __init__(self, code: int):
        self.code = code
        name = self.ERROR_NAMES.get(code, f"Unknown({code})")
        super().__init__(f"apiexec error: {name}")


def _check(rc: int) -> int:
    """Raise ApiExecError for negative return codes."""
    if rc < 0:
        raise ApiExecError(rc)
    return rc


class Stream:
    """A streaming connection to an API via apiexec.

    Usage::

        stream = Stream("generic_rest", '{"base_url": "..."}')
        for batch_json, count in stream:
            print(f"Got {count} records")
        stream.close()

    Or as a context manager::

        with Stream("generic_rest", '{"base_url": "..."}') as s:
            for batch_json, count in s:
                process(batch_json)
    """

    DEFAULT_BUF_SIZE = 1024 * 1024  # 1 MB

    def __init__(self, adapter: str, config_json: str,
                 policy_json: str = "", buf_size: int = 0):
        self._buf_size = buf_size or self.DEFAULT_BUF_SIZE
        policy = policy_json.encode() if policy_json else None
        self._handle = _lib.stream_create(
            adapter.encode(), config_json.encode(), policy
        )
        if not self._handle:
            raise ApiExecError(-7)  # InvalidArg / creation failed

    def has_next(self) -> bool:
        if not self._handle:
            return False
        return _lib.stream_has_next(self._handle) == 1

    def next_batch(self) -> Tuple[bytes, int]:
        """Fetch the next batch. Returns (json_bytes, record_count)."""
        if not self._handle:
            raise ApiExecError(-7)

        buf = ctypes.create_string_buffer(self._buf_size)
        count = ctypes.c_int32(0)
        # Release GIL during the C call
        rc = _lib.stream_next_batch_v1(
            self._handle, buf, self._buf_size, ctypes.byref(count)
        )
        if rc == STREAM_EXHAUSTED:
            raise StopIteration
        _check(rc)
        return buf.value, count.value

    def cancel(self):
        """Cancel the stream. Thread-safe."""
        if self._handle:
            _lib.stream_cancel(self._handle)

    def close(self):
        """Free all C resources. Safe to call multiple times."""
        if self._handle:
            _lib.stream_destroy(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def __iter__(self):
        return self

    def __next__(self) -> Tuple[bytes, int]:
        if not self.has_next():
            raise StopIteration
        return self.next_batch()

    def __del__(self):
        self.close()
