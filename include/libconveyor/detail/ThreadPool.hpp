#pragma once

#include <functional>
#include <memory>

namespace libconveyor {

/**
 * ThreadPool: High-performance, topology-aware compute engine for libconveyor.
 * Using PIMPL to hide citor.hpp templates.
 */
class ThreadPool {
public:
    ThreadPool();
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void parallel_for(size_t first, size_t last, std::function<void(size_t, size_t)> fn);
    void submit_detached(std::function<void()> fn);

    // Explicitly shut down the pool before destruction to avoid affinity races.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace libconveyor
