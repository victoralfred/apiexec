// Example: cost budget enforcement.
//
// Demonstrates CostAwarePolicy via JSON config. The policy halts execution
// when cumulative cost reaches the configured budget. Useful for bounding
// spend on AI API calls.
//
// Note: CostAwarePolicy is set by passing a policy that sets cost hooks.
// Since DefaultPolicy::from_json does not expose budget fields yet,
// this example uses the generic_rest adapter (no token cost) to show
// the ErrBudgetExhausted surface — a real AI adapter (openai/anthropic)
// would report token cost via response_cost() and halt at the budget.
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

	"github.com/voseghale/apiexec/bindings/go/apiexec"
)

func main() {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		page := 0
		if t := r.URL.Query().Get("cursor"); t != "" {
			page, _ = strconv.Atoi(t)
		}

		body := map[string]any{
			"data": []map[string]any{{"id": page, "value": fmt.Sprintf("record_%d", page)}},
			"next": strconv.Itoa(page + 1),  // infinite stream
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

	// Fetch a bounded number of batches and check cost info
	const maxFetches = 5
	for i := 0; i < maxFetches; i++ {
		data, count, fetchErr := stream.NextBatch(0)
		if fetchErr != nil {
			if errors.Is(fetchErr, apiexec.ErrBudgetExhausted) {
				fmt.Println("Budget exhausted — stopping stream")
				break
			}
			fmt.Printf("Error: %v\n", fetchErr)
			break
		}
		fmt.Printf("Fetch %d: %d records (%d bytes)\n", i+1, count, len(data))

		remaining, exceeded, err := stream.CostInfo()
		if err == nil {
			fmt.Printf("  Budget: remaining=%.0f  exceeded=%v\n", remaining, exceeded)
		}
	}

	fmt.Println("\nTip: For real AI cost tracking, use the openai or anthropic")
	fmt.Println("adapter — they report token counts via the response_cost hook,")
	fmt.Println("and a CostAwarePolicy with budget_tokens halts automatically.")
}
