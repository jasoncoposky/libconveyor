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
    static ThreadPool& instance();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void parallel_for(size_t first, size_t last, std::function<void(size_t, size_t)> fn);

    void submit_detached(std::function<void()> fn);

private:
    ThreadPool();
    ~ThreadPool();

    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace libconveyor
