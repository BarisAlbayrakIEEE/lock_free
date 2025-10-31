#ifndef ENUM_STRUCTURE_TYPES_HPP
#define ENUM_STRUCTURE_TYPES_HPP

#include <cstdint>

namespace BA_Concurrency {
    enum class Enum_Structure_Types : std::uint8_t {
        Linked,
        Static_Array,
        Static_Ring_Buffer,
        Dynamic_Array,
        Dynamic_Ring_Buffer };
}

#endif // ENUM_STRUCTURE_TYPES_HPP
