// Package apiexec provides Go bindings for the apiexec streaming execution engine.
//
// It wraps the ABI-stable C API via cgo. The Stream type manages the lifecycle
// of a StreamHandle and provides an idiomatic Go iterator interface.
//
// A Stream must not be used concurrently from multiple goroutines. Call
// Cancel() from any goroutine, but all other methods require external
// synchronisation if shared.
package apiexec

/*
#cgo LDFLAGS: -L${SRCDIR}/../../../build -lapiexec_capi -lapiexec_adapters -lapiexec_transport -lapiexec_policy -lcurl -lstdc++ -lm
#cgo CFLAGS: -I${SRCDIR}/../../../source/c_api
#include "c_api.h"
#include <stdlib.h>
*/
import "C"
import (
	"bytes"
	"errors"
	"fmt"
	"runtime"
	"sync"
	"unsafe"
)

// Error codes matching the C API.
var (
	ErrExhausted  = errors.New("apiexec: stream exhausted")
	ErrRateLimit  = errors.New("apiexec: rate limited")
	ErrServer     = errors.New("apiexec: server error")
	ErrClient     = errors.New("apiexec: client error")
	ErrParse      = errors.New("apiexec: parse error")
	ErrNetwork    = errors.New("apiexec: network error")
	ErrCancelled        = errors.New("apiexec: cancelled")
	ErrInvalidArg       = errors.New("apiexec: invalid argument")
	ErrBudgetExhausted  = errors.New("apiexec: budget exhausted")
)

func errorFromCode(code C.int32_t) error {
	switch code {
	case C.STREAM_OK:
		return nil
	case C.STREAM_EXHAUSTED:
		return ErrExhausted
	case C.STREAM_ERROR_RATE_LIMIT:
		return ErrRateLimit
	case C.STREAM_ERROR_SERVER:
		return ErrServer
	case C.STREAM_ERROR_CLIENT:
		return ErrClient
	case C.STREAM_ERROR_PARSE:
		return ErrParse
	case C.STREAM_ERROR_NETWORK:
		return ErrNetwork
	case C.STREAM_ERROR_CANCELLED:
		return ErrCancelled
	case C.STREAM_ERROR_INVALID_ARG:
		return ErrInvalidArg
	case C.STREAM_ERROR_BUDGET_EXHAUSTED:
		return ErrBudgetExhausted
	default:
		return fmt.Errorf("apiexec: unknown error code %d", code)
	}
}

// Stream wraps a C StreamHandle and provides an idiomatic Go interface.
// A Stream must be closed after use to free C resources.
type Stream struct {
	mu     sync.Mutex
	handle *C.StreamHandle
}

// NewStream creates a new stream for the named adapter with JSON configuration.
// policyJSON may be empty for default policy settings.
func NewStream(adapter, configJSON, policyJSON string) (*Stream, error) {
	cAdapter := C.CString(adapter)
	defer C.free(unsafe.Pointer(cAdapter))

	cConfig := C.CString(configJSON)
	defer C.free(unsafe.Pointer(cConfig))

	var cPolicy *C.char
	if policyJSON != "" {
		cPolicy = C.CString(policyJSON)
		defer C.free(unsafe.Pointer(cPolicy))
	}

	handle := C.stream_create(cAdapter, cConfig, cPolicy)
	if handle == nil {
		return nil, fmt.Errorf("apiexec: stream_create failed for adapter %q", adapter)
	}

	s := &Stream{handle: handle}
	runtime.SetFinalizer(s, (*Stream).Close)
	return s, nil
}

// HasNext returns true if more data is available.
func (s *Stream) HasNext() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.handle == nil {
		return false
	}
	return C.stream_has_next(s.handle) == 1
}

// DefaultBufSize is the default buffer size for NextBatch and NextString.
const DefaultBufSize = 1024 * 1024 // 1 MB

// NextBatch fetches the next batch of records as raw JSON bytes.
// Returns the JSON data and the number of records, or an error.
func (s *Stream) NextBatch(bufSize int) ([]byte, int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.handle == nil {
		return nil, 0, ErrInvalidArg
	}
	if bufSize <= 0 {
		bufSize = DefaultBufSize
	}

	// Allocate C-heap buffer to avoid passing Go heap pointers into C
	cbuf := C.malloc(C.size_t(bufSize))
	if cbuf == nil {
		return nil, 0, fmt.Errorf("apiexec: malloc failed for %d bytes", bufSize)
	}
	defer C.free(cbuf)

	var outCount C.int32_t
	rc := C.stream_next_batch_v1(s.handle, cbuf, C.int32_t(bufSize), &outCount)
	if err := errorFromCode(rc); err != nil {
		return nil, 0, err
	}

	// Copy from C buffer to Go slice
	data := C.GoBytes(cbuf, C.int32_t(findNullTerminator(cbuf, bufSize)))
	return data, int(outCount), nil
}

// NextTimestamp fetches the next record's timestamp in epoch milliseconds.
// For cursor-based adapters without time windows, returns 0.
func (s *Stream) NextTimestamp() (int64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.handle == nil {
		return 0, ErrInvalidArg
	}

	var tsMs C.int64_t
	rc := C.stream_next_v2_ts(s.handle, &tsMs)
	if err := errorFromCode(rc); err != nil {
		return 0, err
	}
	return int64(tsMs), nil
}

// NextString fetches the next record as a JSON string.
func (s *Stream) NextString(bufSize int) (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.handle == nil {
		return "", ErrInvalidArg
	}
	if bufSize <= 0 {
		bufSize = DefaultBufSize
	}

	// Allocate C-heap buffer
	cbuf := C.malloc(C.size_t(bufSize))
	if cbuf == nil {
		return "", fmt.Errorf("apiexec: malloc failed for %d bytes", bufSize)
	}
	defer C.free(cbuf)

	rc := C.stream_next_v2_sc(s.handle, (*C.char)(cbuf), C.int32_t(bufSize))
	if err := errorFromCode(rc); err != nil {
		return "", err
	}

	return C.GoString((*C.char)(cbuf)), nil
}

// ForEach iterates all batches in the stream, calling fn for each one.
// fn receives the raw JSON array bytes for the batch. Return a non-nil
// error from fn to stop iteration early. Returns nil on clean exhaustion.
//
// This is a batch-level loop (one call per page). For per-record iteration,
// use the C API stream_foreach_v1 directly, which delivers individual
// record JSON strings via callback.
func (s *Stream) ForEach(fn func(json []byte) error) error {
	for s.HasNext() {
		data, _, err := s.NextBatch(0)
		if err != nil {
			if errors.Is(err, ErrExhausted) {
				return nil
			}
			return err
		}
		if err := fn(data); err != nil {
			return err
		}
	}
	return nil
}

// CostInfo returns the remaining budget and whether the budget is exceeded.
// RemainingBudget is -1.0 if no budget constraint is active.
func (s *Stream) CostInfo() (remainingBudget float64, budgetExceeded bool, err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.handle == nil {
		return 0, false, ErrInvalidArg
	}

	var cBudget C.double
	var cExceeded C.int32_t
	rc := C.stream_cost_info_v1(s.handle, &cBudget, &cExceeded)
	if err := errorFromCode(rc); err != nil {
		return 0, false, err
	}
	return float64(cBudget), cExceeded != 0, nil
}

// Cancel cancels the stream. Safe to call from any goroutine.
func (s *Stream) Cancel() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.handle != nil {
		C.stream_cancel(s.handle)
	}
}

// Close frees all C resources associated with this stream.
// Safe to call multiple times.
func (s *Stream) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.handle != nil {
		C.stream_destroy(s.handle)
		s.handle = nil
		runtime.SetFinalizer(s, nil)
	}
	return nil
}

// findNullTerminator returns the index of the first null byte in a C buffer.
func findNullTerminator(ptr unsafe.Pointer, maxLen int) int {
	buf := unsafe.Slice((*byte)(ptr), maxLen)
	n := bytes.IndexByte(buf, 0)
	if n < 0 {
		return maxLen
	}
	return n
}
