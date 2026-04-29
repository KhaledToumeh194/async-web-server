# Asynchronous Web Server

A high-performance asynchronous HTTP web server implemented in C, built as part of the Operating Systems course at POLITEHNICA Bucharest.

## Overview

This server handles thousands of concurrent HTTP connections without blocking, using Linux-specific I/O primitives for maximum performance.

## Features

- **epoll** — Event-driven connection handling for scalable concurrency
- **Zero-copy file transfer** — Static files served via `sendfile()` for maximum throughput
- **Async I/O** — Dynamic content read using `libaio` (Linux async I/O) with `eventfd` notifications
- **HTTP parsing** — Incoming requests parsed using `http-parser`
- **Full connection state machine** — Manages the entire lifecycle: receive → parse → respond → teardown
- **Non-blocking sockets** — All I/O operations are non-blocking with proper `EAGAIN` handling

## Technologies

- C, epoll, libaio, sendfile, eventfd, POSIX sockets, HTTP parser

