// Example: cancelling an in-progress stream from another goroutine.
//
// Cancel() is thread-safe. A watchdog goroutine can cancel the stream
// based on a timeout, context, or user signal. The main goroutine sees
// the cancellation as an error on the next NextBatch call.
//
// Usage: go run main.go
package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strconv"
	"time"

	"github.com/voseghale/apiexec/bindings/go/apiexec"
)

func main() {
	// Mock server with infinite pagination and 10ms latency per request
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(10 * time.Millisecond)

		page := 0
		if t := r.URL.Query().Get("cursor"); t != "" {
			page, _ = strconv.Atoi(t)
		}

		body := map[string]any{
			"data": []map[string]any{{"id": page}},
			"next": strconv.Itoa(page + 1),  // infinite
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(body)
	}))
	defer server.Close()

	config := fmt.Sprintf(`{"base_url": %q}`, server.URL)
	stream, err := apiexec.NewStream("generic_rest", config, `{"prefetch_depth": 0}`)
	if err != nil {
		panic(err)
	}
	defer stream.Close()

	// Watchdog goroutine cancels after 200ms
	go func() {
		time.Sleep(200 * time.Millisecond)
		fmt.Println("\n[watchdog] cancelling stream")
		stream.Cancel()
	}()

	count := 0
	for stream.HasNext() {
		_, _, err := stream.NextBatch(0)
		if err != nil {
			if errors.Is(err, apiexec.ErrCancelled) {
				fmt.Printf("Stream cancelled cleanly after %d batches\n", count)
				return
			}
			fmt.Printf("Error: %v\n", err)
			break
		}
		count++
	}
	fmt.Printf("Stream ended after %d batches\n", count)
}
