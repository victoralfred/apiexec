// Example: streaming batches with prefetch.
//
// Compile: javac -cp ".:jna.jar:../src/main/java" StreamingExample.java
// Run:     java  -cp ".:jna.jar:../src/main/java" \
//               -Djna.library.path=../../../build StreamingExample

import com.apiexec.Stream;
import com.sun.net.httpserver.HttpServer;
import com.sun.net.httpserver.HttpExchange;
import java.net.InetSocketAddress;

public class StreamingExample {
    static final int TOTAL_PAGES = 10;
    static final int RECORDS_PER_PAGE = 5;

    public static void main(String[] args) throws Exception {
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        server.createContext("/", (HttpExchange exchange) -> {
            try { Thread.sleep(20); } catch (InterruptedException ignored) {}

            String query = exchange.getRequestURI().getQuery();
            int page = 0;
            if (query != null && query.contains("cursor=")) {
                for (String p : query.split("&")) {
                    if (p.startsWith("cursor=")) page = Integer.parseInt(p.substring(7));
                }
            }

            StringBuilder data = new StringBuilder("[");
            for (int i = 0; i < RECORDS_PER_PAGE; i++) {
                if (i > 0) data.append(",");
                data.append(String.format("{\"id\":%d}", page * RECORDS_PER_PAGE + i));
            }
            data.append("]");

            String next = (page + 1 < TOTAL_PAGES) ? "\"" + (page + 1) + "\"" : "null";
            String json = String.format("{\"data\":%s,\"next\":%s}", data, next);
            byte[] body = json.getBytes();
            exchange.getResponseHeaders().set("Content-Type", "application/json");
            exchange.sendResponseHeaders(200, body.length);
            exchange.getResponseBody().write(body);
            exchange.close();
        });
        server.start();

        int port = server.getAddress().getPort();
        String config = String.format("{\"base_url\":\"http://127.0.0.1:%d\"}", port);
        String policy = "{\"prefetch_depth\":1}";  // enable prefetch

        long start = System.currentTimeMillis();
        int batches = 0;
        int total = 0;

        try (Stream stream = new Stream("generic_rest", config, policy)) {
            while (stream.hasNext()) {
                Stream.BatchResult r = stream.nextBatch();
                if (r == null) break;
                batches++;
                total += r.count();
                Thread.sleep(20);  // simulate processing, overlapped with background prefetch
                System.out.printf("Batch %d: %d records%n", batches, r.count());
            }
        }

        long elapsed = System.currentTimeMillis() - start;
        System.out.printf("%nProcessed %d batches, %d records in %dms%n",
            batches, total, elapsed);
        System.out.println("With prefetch, wall time is ~max(fetch, process) x N.");

        server.stop(0);
    }
}
