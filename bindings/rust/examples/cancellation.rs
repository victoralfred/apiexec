//! Example: cancelling an in-progress stream from another thread.
//!
//! Run: cargo run --example cancellation

use apiexec::{Error, Stream};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

fn handle_client(mut stream: TcpStream, counter: Arc<AtomicI32>) {
    let mut buf = [0u8; 4096];
    let _ = stream.read(&mut buf);
    thread::sleep(Duration::from_millis(10));

    let page = counter.fetch_add(1, Ordering::SeqCst);
    let body = format!(
        r#"{{"data":[{{"id":{}}}],"next":"{}"}}"#,  // infinite stream
        page,
        page + 1
    );
    let resp = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
        body.len(), body
    );
    let _ = stream.write_all(resp.as_bytes());
}

fn main() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let counter = Arc::new(AtomicI32::new(0));

    let counter_clone = counter.clone();
    thread::spawn(move || {
        for stream in listener.incoming() {
            if let Ok(s) = stream {
                handle_client(s, counter_clone.clone());
            }
        }
    });

    let config = format!(r#"{{"base_url":"http://127.0.0.1:{}"}}"#, port);
    let stream = Arc::new(Stream::new("generic_rest", &config, r#"{"prefetch_depth": 0}"#).unwrap());

    // Watchdog thread cancels after 200ms
    let stream_clone = stream.clone();
    thread::spawn(move || {
        thread::sleep(Duration::from_millis(200));
        println!("\n[watchdog] cancelling stream");
        stream_clone.cancel();
    });

    let mut count = 0;
    while stream.has_next() {
        match stream.next_batch(0) {
            Ok(_) => count += 1,
            Err(Error::Cancelled) => {
                println!("Stream cancelled after {} batches", count);
                return;
            }
            Err(e) => {
                println!("Error: {}", e);
                break;
            }
        }
    }
    println!("Stream ended after {} batches", count);
}
