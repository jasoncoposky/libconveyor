## Observability and Monitoring

`libconveyor` provides a rich set of metrics through a "reset-on-read" statistics-gathering mechanism, enabling real-time monitoring, performance tuning, and easier debugging.

### Accessing Statistics

Statistics are accessed via the `conveyor_get_stats()` function (in the C API) or the `stats()` method (in the C++17 wrapper). When called, the function returns a snapshot of the metrics and atomically resets the internal counters to zero. This model is ideal for windowed monitoring, allowing an application to periodically poll the stats to get a clear picture of performance over the last interval.

The `conveyor_stats_t` struct (or `libconveyor::v2::Stats`) provides the following key metrics:

*   **`bytes_written` & `bytes_read`**: The total number of bytes written to and read from the conveyor during the last interval. Useful for calculating throughput.
*   **`avg_write_latency_ms` & `avg_read_latency_ms`**: The average latency of the underlying `pwrite` and `pread` operations, as observed by the worker threads. This helps isolate backend storage performance from the application's perceived latency.
*   **`write_buffer_full_events`**: A counter that increments each time a `conveyor_write` call has to wait because the write buffer is full. A consistently high value indicates that the application is producing data faster than the backend storage can consume it.
*   **`last_error_code`**: Captures the `errno` of the first I/O error that occurs in a background worker thread. This "sticky" error code is crucial for diagnosing otherwise silent backend failures.

### Potential Use Cases

The observability features of `libconveyor` unlock several powerful use cases:

1.  **Real-time Performance Monitoring:**
    *   **Dashboarding:** The stats can be exported to a time-series database (e.g., Prometheus, InfluxDB) and visualized in a dashboard (e.g., Grafana) to provide a real-time view of I/O throughput, latency, and buffer health.
    *   **Alerting:** Automated alerts can be configured to trigger on key metrics. For example, an alert could be fired if `avg_write_latency_ms` exceeds a certain threshold, if `write_buffer_full_events` is persistently high, or if `last_error_code` becomes non-zero.

2.  **Adaptive Application Behavior:**
    *   **Dynamic Resource Tuning:** An application can monitor `write_buffer_full_events`. If this value is consistently high, it may indicate that the initial buffer size is too small for the current workload, and the application could choose to re-create the conveyor with a larger `max_write_capacity`.
    *   **Load Shedding:** If write latency is consistently high, an application could implement a backpressure mechanism, temporarily reducing the rate of writes to allow the conveyor's background worker to catch up.

3.  **Debugging and Troubleshooting:**
    *   **Pinpointing Bottlenecks:** The metrics help distinguish between application-level performance and backend storage performance. High `avg_write_latency_ms`, for instance, strongly suggests a bottleneck in the underlying storage system, not in the application logic itself.
    *   **Diagnosing Silent Failures:** The `last_error_code` is invaluable for immediately detecting and diagnosing I/O errors that occur asynchronously in the background, which might otherwise go unnoticed until much later.

### C++17 Monitoring Example

The modern C++ interface makes it trivial to set up a simple monitoring thread:

```cpp
#include "libconveyor/conveyor_modern.hpp"
#include <iostream>
#include <thread>
#include <chrono>

void monitor_conveyor(libconveyor::v2::Conveyor& conveyor, std::atomic<bool>& stop) {
    while (!stop) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        auto stats = conveyor.stats();
        
        std::cout << "\n--- Conveyor Stats (last 5s) ---\"n"
                  << "Bytes Written: " << stats.bytes_written << "\n"
                  << "Bytes Read: " << stats.bytes_read << "\n"
                  << "Avg Write Latency: " << stats.avg_write_latency.count() << " ms\n"
                  << "Avg Read Latency: " << stats.avg_read_latency.count() << " ms\n"
                  << "Write Buffer Full Events: " << stats.write_buffer_full_events << "\n"
                  << "Last Error: " << stats.last_error_code << "\n"
                  << "---------------------------------\n\n";
    }
}
```