#ifndef LIBCONVEYOR_MODERN_HPP
#define LIBCONVEYOR_MODERN_HPP

#include "libconveyor/conveyor.h"
#include <memory>         // std::unique_ptr
#include <string>
#include <vector>
#include <array>
#include <system_error>   // std::error_code
#include <variant>        // C++17
#include <chrono>         // std::chrono
#include <type_traits>    // std::enable_if, std::void_t

namespace libconveyor {
namespace v2 {

using namespace std::chrono_literals;

// --- 1. Result Type (Poor man's std::expected) ---
template <typename T>
class Result {
    std::variant<std::error_code, T> storage_;
public:
    Result(T val) : storage_(std::move(val)) {}
    Result(std::error_code ec) : storage_(ec) {}

    bool has_value() const { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const { return has_value(); }

    T& value() { return std::get<T>(storage_); }
    const T& value() const { return std::get<T>(storage_); }
    
    std::error_code error() const { 
        if (has_value()) return std::error_code();
        return std::get<std::error_code>(storage_); 
    }
};

// Specialization for void (success/failure only)
template <>
class Result<void> {
    std::error_code ec_;
public:
    Result() : ec_() {}
    Result(std::error_code ec) : ec_(ec) {}
    
    bool has_value() const { return !ec_; }
    explicit operator bool() const { return !ec_; }
    std::error_code error() const { return ec_; }
};

// --- 2. SFINAE Detection for Contiguous Containers ---
// Detects objects that have .data() and .size()
template <typename T, typename = void>
struct is_contiguous : std::false_type {};

template <typename T>
struct is_contiguous<T, std::void_t<
    decltype(std::data(std::declval<T>())), 
    decltype(std::size(std::declval<T>()))
>> : std::true_type {};

// --- 3. Configuration Struct ---
struct Config {
    storage_handle_t handle;
    storage_operations_t ops;
    size_t write_capacity = 1024 * 1024;
    size_t read_capacity = 1024 * 1024;
    int open_flags = O_RDWR;
};

// --- 4. The Modern Conveyor Class ---
class Conveyor {
private:
    struct Deleter { 
        void operator()(conveyor_t* ptr) const { conveyor_destroy(ptr); } 
    };
    std::unique_ptr<conveyor_t, Deleter> impl_;

public:
    Conveyor() = delete;
    
    // Explicit ownership transfer
    explicit Conveyor(conveyor_t* raw) : impl_(raw) {}
    
    // Move-only type
    Conveyor(Conveyor&&) noexcept = default;
    Conveyor& operator=(Conveyor&&) noexcept = default;
    Conveyor(const Conveyor&) = delete;
    Conveyor& operator=(const Conveyor&) = delete;

    // Factory
    static Result<Conveyor> create(const Config& cfg_v2) {
        conveyor_config_t cfg_c = {0};
        cfg_c.handle = cfg_v2.handle;
        cfg_c.flags = cfg_v2.open_flags;
        cfg_c.ops = cfg_v2.ops;
        cfg_c.initial_write_size = cfg_v2.write_capacity;
        cfg_c.initial_read_size = cfg_v2.read_capacity;
        cfg_c.max_write_size = cfg_v2.write_capacity; // For now, initial size is max
        cfg_c.max_read_size = cfg_v2.read_capacity;   // For now, initial size is max
        
        conveyor_t* raw = conveyor_create(&cfg_c);
        if (!raw) {
            return std::error_code(errno, std::system_category());
        }
        return Conveyor(raw);
    }

    // --- Modern Write API ---
    // Accepts std::vector, std::string, std::array, etc.
    template <typename Container, 
              typename = std::enable_if_t<is_contiguous<Container>::value>>
    Result<size_t> write(const Container& buffer) {
        const void* ptr = std::data(buffer);
        // C++17 std::size returns number of elements, we need byte size
        size_t len = std::size(buffer) * sizeof(typename Container::value_type);
        
        ssize_t res = conveyor_write(impl_.get(), ptr, len);
        
        if (res == LIBCONVEYOR_ERROR) {
            return std::error_code(errno, std::system_category());
        }
        return static_cast<size_t>(res);
    }

    // --- Modern Read API ---
    // Accepts mutable vector/string/array to fill
    template <typename Container,
              typename = std::enable_if_t<is_contiguous<Container>::value>>
    Result<size_t> read(Container& buffer) {
        void* ptr = (void*)std::data(buffer);
        size_t len = std::size(buffer) * sizeof(typename Container::value_type);

        ssize_t res = conveyor_read(impl_.get(), ptr, len);
        
        if (res == LIBCONVEYOR_ERROR) {
            return std::error_code(errno, std::system_category());
        }
        return static_cast<size_t>(res);
    }

    // --- Seek ---
    Result<off_t> seek(off_t offset, int whence = SEEK_SET) {
        off_t res = conveyor_lseek(impl_.get(), offset, whence);
        if (res == LIBCONVEYOR_ERROR) {
            return std::error_code(errno, std::system_category());
        }
        return res;
    }

    // --- Flush ---
    Result<void> flush() {
        if (conveyor_flush(impl_.get()) != 0) {
            return std::error_code(errno, std::system_category());
        }
        return Result<void>();
    }

    // --- Stats ---
    struct Stats {
        size_t bytes_written;
        size_t bytes_read;
        std::chrono::milliseconds avg_write_latency;
        std::chrono::milliseconds avg_read_latency;
    };

    Stats stats() {
        conveyor_stats_t raw;
        conveyor_get_stats(impl_.get(), &raw);
        return Stats{
            raw.bytes_written,
            raw.bytes_read,
            std::chrono::milliseconds(raw.avg_write_latency_ms),
            std::chrono::milliseconds(raw.avg_read_latency_ms)
        };
    }
};

} // namespace v2
} // namespace libconveyor

#endif // LIBCONVEYOR_MODERN_HPP
