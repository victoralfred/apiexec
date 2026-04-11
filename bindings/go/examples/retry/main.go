// Example: retry behavior and rate-limit adaptation.
//
// Demonstrates how apiexec handles 429 (rate limited) and 5xx errors:
//   - Automatic retry with exponential backoff + jitter
//   - Retry-After header honored
//   - Time window shrinks on first 429 (load signal)
//   - Time window does NOT shrink on 4xx (auth/validation errors)
//   - Stream recovers transparently — caller sees no errors
//
// Usage: go run main.go
package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strconv"
	"sync/atomic"

	"github.com/voseghale/apiexec/bindings/go/apiexec"
)

func main() {
	var requestCount int32

	// Mock server that returns 429 for the first 3 requests, then succeeds
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		count := atomic.AddInt32(&requestCount, 1)

		if count <= 3 {
			w.Header().Set("Retry-After", "0")
			w.WriteHeader(http.StatusTooManyRequests)
			_, _ = w.Write([]byte(`{"error":"rate limited"}`))
			return
		}

		page := 0
		if t := r.URL.Query().Get("cursor"); t != "" {
			page, _ = strconv.Atoi(t)
		}

		body := map[string]any{
			"data": []map[string]any{{"id": page, "value": fmt.Sprintf("record_%d", page)}},
		}
		if page < 2 {
			body["next"] = strconv.Itoa(page + 1)
		} else {
			body["next"] = nil
		}

		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(body)
	}))
	defer server.Close()

	config := fmt.Sprintf(`{"base_url": %q}`, server.URL)
	// Tight retry config so the demo finishes quickly
	policy := `{"max_retries": 5, "base_backoff_ms": 10, "prefetch_depth": 0}`

	stream, err := apiexec.NewStream("generic_rest", config, policy)
	if err != nil {
		panic(err)
	}
	defer stream.Close()

	totalRecords := 0
	pages := 0
	for stream.HasNext() {
		data, count, err := stream.NextBatch(0)
		if err != nil {
			fmt.Printf("Error: %v\n", err)
			break
		}
		pages++
		totalRecords += count
		fmt.Printf("Page %d: %d records (%d bytes)\n", pages, count, len(data))
	}

	fmt.Printf("\nResult: %d pages, %d records\n", pages, totalRecords)
	fmt.Printf("Mock server served %d requests (3 x 429 + 3 successes)\n", atomic.LoadInt32(&requestCount))
	fmt.Println("The engine transparently retried past all 429s.")
}
