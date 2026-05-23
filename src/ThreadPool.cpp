#include "libconveyor/detail/ThreadPool.hpp"
#include <citor.hpp>
#include <thread>
#include <mutex>

namespace libconveyor {

// Use a true persistent singleton to avoid destruction races in citor
std::shared_ptr<ThreadPool> ThreadPool::get_shared_instance() {
    static std::shared_ptr<ThreadPool> instance = std::make_shared<ThreadPool>();
    return instance;
}

struct ThreadPool::Impl {
    citor::ThreadPool pool;

    explicit Impl(size_t threads) : pool(threads) {}
};

ThreadPool::ThreadPool() {
    size_t threads = std::thread::hardware_concurrency();
    if (threads == 0) threads = 4;
    pimpl_ = std::make_unique<Impl>(threads);
}

ThreadPool::~ThreadPool() = default;

void ThreadPool::parallel_for(size_t first, size_t last, std::function<void(size_t, size_t)> fn) {
    if (pimpl_) pimpl_->pool.template parallelFor<citor::HintsDefaults>(first, last, std::move(fn));
}

void ThreadPool::submit_detached(std::function<void()> fn) {
    if (pimpl_) pimpl_->pool.template submitDetached<citor::HintsDefaults>(std::move(fn));
}

void ThreadPool::shutdown() {
    // For a persistent singleton, shutdown is rarely needed but we keep it for completeness
    pimpl_.reset();
}

} // namespace libconveyor
