<p align="center">
  <img src="assets/libconveyor_logo.jpg" alt="libconveyor Logo" width="500"/>
</p>

# libconveyor: High-Performance I/O Buffering Library

[![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

`libconveyor` is a cross-platform C++20 library designed to significantly accelerate I/O operations by implementing a highly efficient, asynchronous, and adaptive buffering layer between the application and the underlying storage system.

## Key Features

- **Asynchronous Handoff**: Offloads slow I/O operations to a high-performance compute foundation.
- **Compute Foundation (Citor)**: Uses the **citor** high-performance thread pool for sub-microsecond task dispatch and hardware-topology awareness.
- **Lock-Free Offloading**: Integrated **moodycamel::ConcurrentQueue** for zero-latency metadata offloading from application threads.
- **Extreme Scalability**: Efficiently handles thousands of simultaneous I/O streams using a fixed number of threads, avoiding OS thread exhaustion.
- **Topology-Aware**: Pins I/O tasks to specific Core Complex Dies (CCDs) to maximize L3 cache hit rates during data movement.
- **Adaptive Buffer Management**: Automatically scales write and read buffers based on the application's access patterns and backend latency.
- **Read-Ahead & Prefetching**: Predicts and proactively fetches data from the storage backend to serve application reads instantly from memory.
- **Unified C/C++ API**: Provides a stable C-style interface and a modern, type-safe C++20 wrapper (with RAII and `Result<T>` error handling).
- **Comprehensive Observability**: Offers a rich set of "reset-on-read" metrics for real-time monitoring and tuning.

## Performance & Scalability

`libconveyor` is optimized for both high-throughput single-stream I/O and extreme multi-file scalability.

### Single-Stream Benchmarks
*Simulated Backend Latency: 2 ms*

| Metric | Raw POSIX | libconveyor | Speedup |
| :--- | :--- | :--- | :--- |
| **Write Throughput** | 0.24 MB/s | **6800.41 MB/s** | **~28,000x** |
| **Read Throughput** | 0.24 MB/s | **214.63 MB/s** | **~890x** |

### Extreme Scalability (2,000 Simultaneous Streams)
*Stress test opening 2,000 conveyors on a single host.*

| Foundation | Total Time | Throughput | Result |
| :--- | :--- | :--- | :--- |
| **Dedicated Threads** | 14.9s | 16.7 MB/s | System Unstable |
| **Citor Tasks** | **2.5s** | **97.8 MB/s** | **~6x Speedup** |

---

## Installation & Usage

### 1. Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 2. Basic C Usage
```c
#include "libconveyor/conveyor.h"

// ... setup storage_operations_t ...
conveyor_config_t cfg = { .handle = fd, .ops = ops, .initial_write_size = 1024*1024 };
conveyor_t* conv = conveyor_create(&cfg);

conveyor_write(conv, "Hello Mesh", 11);
conveyor_flush(conv);
conveyor_destroy(conv);
```

Detailed API documentation is available in [API.md](API.md).

## License

`libconveyor` is distributed under the **BSD 3-Clause License**. See the [LICENSE.md](./LICENSE.md) file for full details.
