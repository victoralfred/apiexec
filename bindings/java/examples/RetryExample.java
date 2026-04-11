// Example: retry and rate-limit adaptation.
//
// Starts a local mock server that returns 3 x 429 before succeeding.
// The engine retries transparently and the caller never sees 429s.
//
// Compile: javac -cp ".:jna.jar:../src/main/java" RetryExample.java
// Run:     java  -cp ".:jna.jar:../src/main/java" \
//               -Djna.library.path=../../../build RetryExample

import com.apiexec.Stream;
import com.sun.net.httpserver.HttpServer;
import com.sun.net.httpserver.HttpHandler;
import com.sun.net.httpserver.HttpExchange;
import java.net.InetSocketAddress;
import java.util.concurrent.atomic.AtomicInteger;

public class RetryExample {
    public static void main(String[] args) throws Exception {
        AtomicInteger requestCount = new AtomicInteger(0);

        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        server.createContext("/", (HttpExchange exchange) -> {
            int count = requestCount.incrementAndGet();
            if (count <= 3) {
                byte[] body = "{\"error\":\"rate limited\"}".getBytes();
                exchange.getResponseHeaders().set("Retry-After", "0");
                exchange.sendResponseHeaders(429, body.length);
                exchange.getResponseBody().write(body);
                exchange.close();
                return;
            }

            String query = exchange.getRequestURI().getQuery();
            int page = 0;
            if (query != null && query.contains("cursor=")) {
                for (String p : query.split("&")) {
                    if (p.startsWith("cursor=")) page = Integer.parseInt(p.substring(7));
                }
            }

            String next = page < 2 ? "\"" + (page + 1) + "\"" : "null";
            String json = String.format(
                "{\"data\":[{\"id\":%d}],\"next\":%s}", page, next);
            byte[] body = json.getBytes();
            exchange.getResponseHeaders().set("Content-Type", "application/json");
            exchange.sendResponseHeaders(200, body.length);
            exchange.getResponseBody().write(body);
            exchange.close();
        });
        server.start();

        int port = server.getAddress().getPort();
        String config = String.format("{\"base_url\":\"http://127.0.0.1:%d\"}", port);
        String policy = "{\"max_retries\":5,\"base_backoff_ms\":10,\"prefetch_depth\":0}";

        try (Stream stream = new Stream("generic_rest", config, policy)) {
            int pages = 0;
            int records = 0;
            while (stream.hasNext()) {
                Stream.BatchResult r = stream.nextBatch();
                if (r == null) break;
                pages++;
                records += r.count();
                System.out.printf("Page %d: %d records (%d bytes)%n",
                    pages, r.count(), r.data().length);
            }
            System.out.printf("%nResult: %d pages, %d records%n", pages, records);
            System.out.printf("Mock server served %d requests (3 x 429 + 3 successes)%n",
                requestCount.get());
        }

        server.stop(0);
    }
}
