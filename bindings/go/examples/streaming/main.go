// Example: record-level streaming with ForEach.
//
// Demonstrates the ForEach interface for iterating batches as they arrive.
// Works with prefetch enabled so I/O overlaps with processing.
//
// Usage: go run main.go
package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strconv"
	"time"

	"github.com/voseghale/apiexec/bindings/go/apiexec"
)

func main() {
	const totalPages = 10
	const recordsPerPage = 5

	// Mock server with simulated latency
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(20 * time.Millisecond) // simulate API latency

		page := 0
		if t := r.URL.Query().Get("cursor"); t != "" {
			page, _ = strconv.Atoi(t)
		}

		data := make([]map[string]any, recordsPerPage)
		for i := range data {
			data[i] = map[string]any{
				"id":    page*recordsPerPage + i,
				"value": fmt.Sprintf("record_%d_%d", page, i),
			}
		}

		body := map[string]any{"data": data}
		if page+1 < totalPages {
			body["next"] = strconv.Itoa(page + 1)
		} else {
			body["next"] = nil
		}

		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(body)
	}))
	defer server.Close()

	config := fmt.Sprintf(`{"base_url": %q}`, server.URL)
	// Prefetch enabled: next batch fetched in background while we process current
	stream, err := apiexec.NewStream("generic_rest", config, `{"prefetch_depth": 1}`)
	if err != nil {
		panic(err)
	}
	defer stream.Close()

	start := time.Now()
	batchCount := 0
	totalRecords := 0

	err = stream.ForEach(func(data []byte) error {
		batchCount++
		// Parse and count records
		var batch []json.RawMessage
		if err := json.Unmarshal(data, &batch); err != nil {
			return err
		}
		totalRecords += len(batch)

		// Simulate processing time — overlapped with background prefetch
		time.Sleep(20 * time.Millisecond)

		fmt.Printf("Batch %d: %d records\n", batchCount, len(batch))
		return nil
	})
	if err != nil {
		fmt.Printf("ForEach error: %v\n", err)
	}

	elapsed := time.Since(start)
	fmt.Printf("\nProcessed %d batches, %d records in %v\n", batchCount, totalRecords, elapsed)
	fmt.Println("With prefetch, wall time is roughly max(fetch, process) x N,")
	fmt.Println("not fetch + process. Sequential mode would take ~2x longer.")
}
