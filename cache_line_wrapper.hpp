#ifndef CACHE_LINE_WRAPPER_HPP
#define CACHE_LINE_WRAPPER_HPP

#include <atomic>
#include <type_traits>

namespace BA_Concurrency {
    // A wrapper for cache line alignment
    template <typename T>
    requires std::is_default_constructible_v<T>
    struct alignas(std::hardware_destructive_interference_size) cache_line_wrapper {
        T value;
    };
}

#endif // CACHE_LINE_WRAPPER_HPP
