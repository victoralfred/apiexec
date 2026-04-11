// Example: using the adapter registry to switch between adapters at runtime.
//
// The same C API — stream_create(adapter_name, config, policy) — dispatches
// to any of the registered adapters: generic_rest, datadog_metrics, openai,
// anthropic. The caller picks the adapter by name.
//
// Usage: go run main.go
package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"

	"github.com/voseghale/apiexec/bindings/go/apiexec"
)

func main() {
	// Mock OpenAI-compatible server
	openaiMock := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		resp := map[string]any{
			"choices": []map[string]any{
				{"index": 0, "message": map[string]string{
					"role": "assistant", "content": "Hello from mock OpenAI",
				}},
			},
			"usage": map[string]int{
				"prompt_tokens": 10, "completion_tokens": 20, "total_tokens": 30,
			},
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(resp)
	}))
	defer openaiMock.Close()

	// Mock Datadog-compatible server
	datadogMock := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		resp := map[string]any{
			"series": []map[string]any{
				{
					"metric":    "system.cpu.user",
					"pointlist": [][]float64{{1700000000, 42.5}, {1700000060, 43.1}},
				},
			},
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(resp)
	}))
	defer datadogMock.Close()

	runAdapter := func(name, config, policy string) {
		fmt.Printf("--- %s ---\n", name)
		stream, err := apiexec.NewStream(name, config, policy)
		if err != nil {
			fmt.Printf("  Error: %v\n", err)
			return
		}
		defer stream.Close()

		if !stream.HasNext() {
			fmt.Println("  No data available")
			return
		}

		data, count, err := stream.NextBatch(0)
		if err != nil {
			fmt.Printf("  Fetch error: %v\n", err)
			return
		}
		fmt.Printf("  Got %d records (%d bytes)\n", count, len(data))
		if len(data) > 120 {
			fmt.Printf("  Preview: %s...\n", data[:120])
		} else {
			fmt.Printf("  Data: %s\n", data)
		}
		fmt.Println()
	}

	// OpenAI adapter
	openaiConfig, _ := json.Marshal(map[string]any{
		"base_url":   openaiMock.URL,
		"api_key":    "fake-key",
		"model":      "gpt-4",
		"prompts":    []string{"Say hello"},
		"max_tokens": 50,
	})
	runAdapter("openai", string(openaiConfig), `{"prefetch_depth": 0}`)

	// Datadog adapter
	datadogConfig, _ := json.Marshal(map[string]any{
		"base_url":  datadogMock.URL,
		"api_key":   "fake-key",
		"app_key":   "fake-key",
		"query":     "avg:system.cpu.user{*}",
		"window_ms": 3600000,
	})
	runAdapter("datadog_metrics", string(datadogConfig), `{"prefetch_depth": 0}`)

	fmt.Println("Same stream_create API drives any adapter. The registry")
	fmt.Println("dispatches at runtime — no recompilation needed to switch.")
}
