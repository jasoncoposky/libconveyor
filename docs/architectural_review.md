# libconveyor Architectural Review & Implementation Plan

## 1. Executive Summary
`libconveyor` is designed to be an extreme-performance I/O buffering layer. Recent audits revealed that while the asynchronous backend is functional, the "hot path" (API layer) suffers from significant mutex contention and architectural drift. This plan outlines the transition to a truly parallel, lock-free reservation system and ensures strict read-after-write consistency via the "Snoop Pattern."

## 2. Core Principles
*   **Zero-Copy Handoff:** Minimize memory moves. Application data should move directly into hardware-aligned segment pools.
*   **Zero-Latency Reservation:** Producers must never block on I/O. Space reservation should be atomic and wait-free.
*   **Snoop Consistency:** Reads must "snoop" the write-ahead queue to ensure they see unflushed data without requiring a global flush.
*   **Hardware Sympathy:** Align segments to cache lines (64 bytes) and pages (4KB) to maximize DMA efficiency and minimize TLB misses.

## 3. Architectural Audit Findings
| Component | Current Status | Targeted Improvement |
| :--- | :--- | :--- |
| **Write Path** | Global Mutex during `memcpy`. | Atomic "Reserve-and-Copy" with ref-counting. |
| **Read Path** | Synchronous wait on empty cache. | Proactive prefetching & non-blocking snoop. |
| **Consistency** | Incomplete (Stale reads possible). | Full "Snoop Pattern" implementation. |
| **Lifecycle** | Fragile (yield-loops). | Robust signal/join via `std::condition_variable`. |
| **O_APPEND** | Stale end-of-file tracking. | Atomic logical offset tracking. |

## 4. Proposed Solution: The "Reserve-and-Copy" Write Path

### 4.1. Atomic Reservation
Instead of holding a mutex during `memcpy`, `conveyor_write` will:
1.  **Reserve:** Atomically increment a `reservation_cursor` and a `writer_ref_count` on the active segment.
2.  **Copy:** Perform `memcpy` into the reserved slice *outside* the mutex.
3.  **Release:** Atomically decrement `writer_ref_count`. If `ref_count == 0` AND the segment is marked `full`, enqueue it to the `write_queue`.

### 4.2. Segment Rotation
A lightweight mutex will still be used to manage segment rotation (swapping the `active_seg` when full), but this will be off the hot path for most writes.

## 5. Implementation Roadmap

### Phase 1: High-Concurrency Write Path
*   Implement `Segment` metadata structure (cursor, ref-count, state).
*   Refactor `conveyor_write` to use atomic reservation.
*   Update `writeWorkerTask` to handle ref-counted segments.

### Phase 2: Snoop Consistency
*   Implement the "Snoop" logic in `conveyor_read`.
*   Verify with the `test_read_sees_unflushed_write` test case.

### Phase 3: Reliability & Lifecycle
*   Fix `O_APPEND` initialization and atomic tracking.
*   Replace yield-loops in `conveyor_destroy` with proper synchronization.
*   Implement `conveyor_get_stats` and `conveyor_clear_error` stubs.

## 6. Verification Plan
*   **Performance:** Run `conveyor_multi_benchmark` (to be created) with 16+ threads to verify linear scaling.
*   **Correctness:** Execute the full GTest suite, ensuring no timeouts or data mismatches.
*   **Safety:** Run under ThreadSanitizer to detect potential race conditions in the new atomic logic.
