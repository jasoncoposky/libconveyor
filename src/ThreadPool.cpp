#include "libconveyor/detail/ThreadPool.hpp"
#include <citor.hpp>
#include <thread>

namespace libconveyor {

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
    pimpl_.reset();
}

} // namespace libconveyor
