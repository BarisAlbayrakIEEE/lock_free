#ifndef ENUM_CONCURRENCY_MODELS_HPP
#define ENUM_CONCURRENCY_MODELS_HPP

#include <cstdint>

namespace BA_Concurrency {
    enum class Enum_Concurrency_Models : std::uint8_t { SPSC, SPMC, MPSC, MPMC };
}

#endif // ENUM_CONCURRENCY_MODELS_HPP
