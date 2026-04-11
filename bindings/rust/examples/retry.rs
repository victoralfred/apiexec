//! Example: retry behaviour and rate-limit adaptation.
//!
//! Demonstrates how apiexec handles 429 and 5xx errors transparently.
//! Run: cargo run --example retry

use apiexec::{Error, Stream};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Arc;
use std::thread;

fn handle_client(mut stream: TcpStream, counter: Arc<AtomicI32>) {
    let mut buf = [0u8; 4096];
    let _ = stream.read(&mut buf);

    let count = counter.fetch_add(1, Ordering::SeqCst) + 1;
    let (status, body) = if count <= 3 {
        (
            "429 Too Many Requests",
            String::from(r#"{"error":"rate limited"}"#),
        )
    } else {
        let page = count - 4;
        let next = if page < 2 {
            format!(r#""{}""#, page + 1)
        } else {
            "null".to_string()
        };
        (
            "200 OK",
            format!(
                r#"{{"data":[{{"id":{},"v":"r{}"}}],"next":{}}}"#,
                page, page, next
            ),
        )
    };

    let retry_after = if count <= 3 { "Retry-After: 0\r\n" } else { "" };
    let resp = format!(
        "HTTP/1.1 {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n{}\r\n{}",
        status,
        body.len(),
        retry_after,
        body
    );
    let _ = stream.write_all(resp.as_bytes());
}

fn main() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let counter = Arc::new(AtomicI32::new(0));

    let counter_clone = counter.clone();
    let server_thread = thread::spawn(move || {
        // Serve ~10 requests then stop
        let mut served = 0;
        for stream in listener.incoming() {
            if served >= 10 { break; }
            if let Ok(s) = stream {
                handle_client(s, counter_clone.clone());
                served += 1;
            }
        }
    });

    let config = format!(r#"{{"base_url":"http://127.0.0.1:{}"}}"#, port);
    let policy = r#"{"max_retries": 5, "base_backoff_ms": 10, "prefetch_depth": 0}"#;

    let stream = Stream::new("generic_rest", &config, policy).unwrap();

    let mut pages = 0;
    let mut records = 0;
    while stream.has_next() {
        match stream.next_batch(0) {
            Ok((data, count)) => {
                pages += 1;
                records += count;
                println!("Page {}: {} records ({} bytes)", pages, count, data.len());
            }
            Err(Error::Cancelled) => break, // exhausted
            Err(e) => {
                println!("Error: {}", e);
                break;
            }
        }
    }

    println!(
        "\nResult: {} pages, {} records ({} requests, including 3 x 429)",
        pages,
        records,
        counter.load(Ordering::SeqCst)
    );

    drop(server_thread);
}
