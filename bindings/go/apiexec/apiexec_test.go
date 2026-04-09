package apiexec

import (
	"errors"
	"testing"
)

func TestNewStreamUnknownAdapter(t *testing.T) {
	s, err := NewStream("nonexistent", `{"base_url":"http://localhost"}`, "")
	if err == nil {
		s.Close()
		t.Fatal("expected error for unknown adapter")
	}
}

func TestNewStreamInvalidJSON(t *testing.T) {
	s, err := NewStream("generic_rest", "not json", "")
	if err == nil {
		s.Close()
		t.Fatal("expected error for invalid JSON")
	}
}

func TestNewStreamValid(t *testing.T) {
	s, err := NewStream("generic_rest", `{"base_url":"http://localhost:9999"}`, "")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	defer s.Close()

	if !s.HasNext() {
		t.Fatal("expected HasNext to be true for a new stream")
	}
}

func TestStreamCancel(t *testing.T) {
	s, err := NewStream("generic_rest", `{"base_url":"http://localhost:9999"}`,
		`{"prefetch_depth": 0}`)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	defer s.Close()

	s.Cancel()
	if s.HasNext() {
		t.Fatal("expected HasNext to be false after cancel")
	}
}

func TestStreamCloseIdempotent(t *testing.T) {
	s, err := NewStream("generic_rest", `{"base_url":"http://localhost:9999"}`, "")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	// Close multiple times — should not panic
	s.Close()
	s.Close()
	s.Close()
}

func TestStreamNextBatchNetworkError(t *testing.T) {
	// Points at a non-existent server — will get a network error after retries
	s, err := NewStream("generic_rest", `{"base_url":"http://127.0.0.1:1"}`,
		`{"max_retries": 0, "prefetch_depth": 0}`)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	defer s.Close()

	_, _, fetchErr := s.NextBatch(0)
	if fetchErr == nil {
		t.Fatal("expected error from NextBatch")
	}
	if !errors.Is(fetchErr, ErrNetwork) {
		t.Fatalf("expected ErrNetwork, got: %v", fetchErr)
	}
}

func TestStreamWithPolicyJSON(t *testing.T) {
	s, err := NewStream("generic_rest", `{"base_url":"http://localhost:9999"}`,
		`{"max_retries": 2, "prefetch_depth": 0}`)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	defer s.Close()

	if !s.HasNext() {
		t.Fatal("expected HasNext true")
	}
}
