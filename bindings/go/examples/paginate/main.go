// Example: paginate a REST API using apiexec as backend.
//
// Usage: go run main.go <base_url>
// Example: go run main.go http://localhost:8080/api/data
package main

import (
	"fmt"
	"os"

	"github.com/voseghale/apiexec/bindings/go/apiexec"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr, "Usage: %s <base_url>\n", os.Args[0])
		os.Exit(1)
	}

	baseURL := os.Args[1]
	config := fmt.Sprintf(`{"base_url": %q, "page_size": 100}`, baseURL)

	stream, err := apiexec.NewStream("generic_rest", config, `{"prefetch_depth": 1}`)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error creating stream: %v\n", err)
		os.Exit(1)
	}
	defer stream.Close()

	totalRecords := 0
	pages := 0

	for stream.HasNext() {
		data, count, err := stream.NextBatch(0)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error fetching batch: %v\n", err)
			break
		}
		pages++
		totalRecords += count
		fmt.Printf("Page %d: %d records (%d bytes)\n", pages, count, len(data))
	}

	fmt.Printf("\nTotal: %d pages, %d records\n", pages, totalRecords)
}
