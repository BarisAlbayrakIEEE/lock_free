#ifndef ENUM_MEMORY_RECLAIMERS_HPP
#define ENUM_MEMORY_RECLAIMERS_HPP

#include <cstdint>

namespace BA_Concurrency {
    enum class Enum_Memory_Reclaimers : std::uint8_t {
        Reference_Count,
        Hazard_Ptr,
        Epoc_Based };
}

#endif // ENUM_MEMORY_RECLAIMERS_HPP
