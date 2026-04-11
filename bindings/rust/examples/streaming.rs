//! Example: iterating streamed batches with for_each.
//!
//! Run: cargo run --example streaming

use apiexec::Stream;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

const TOTAL_PAGES: i32 = 8;
const RECORDS_PER_PAGE: i32 = 5;

fn handle_client(mut stream: TcpStream, counter: Arc<AtomicI32>) {
    let mut buf = [0u8; 4096];
    let _ = stream.read(&mut buf);
    thread::sleep(Duration::from_millis(20)); // simulate latency

    let page = counter.fetch_add(1, Ordering::SeqCst);
    let mut data = String::from("[");
    for i in 0..RECORDS_PER_PAGE {
        if i > 0 { data.push(','); }
        data.push_str(&format!(
            r#"{{"id":{},"value":"r{}_{}"}}"#,
            page * RECORDS_PER_PAGE + i,
            page,
            i
        ));
    }
    data.push(']');

    let next = if page + 1 < TOTAL_PAGES {
        format!(r#""{}""#, page + 1)
    } else {
        "null".to_string()
    };
    let body = format!(r#"{{"data":{},"next":{}}}"#, data, next);
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
    let stream = Stream::new("generic_rest", &config, r#"{"prefetch_depth": 1}"#).unwrap();

    let start = Instant::now();
    let mut batches = 0;
    let mut total = 0;

    stream
        .for_each(|data, count| {
            batches += 1;
            total += count;
            println!("Batch {}: {} records", batches, count);
            // Simulate processing — overlapped with background prefetch
            thread::sleep(Duration::from_millis(20));
            let _ = data;
            true
        })
        .unwrap();

    println!(
        "\nProcessed {} batches, {} records in {:?}",
        batches,
        total,
        start.elapsed()
    );
}
