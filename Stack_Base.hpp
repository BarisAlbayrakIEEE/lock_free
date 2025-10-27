#ifndef STACK_BASE_HPP
#define STACK_BASE_HPP

#include <cstddef>
#include <type_traits>
#include <variant>
#include "enum_structure_types.hpp"
#include "enum_concurrency_models.hpp"

namespace BA_Concurrency {
    template <
        bool Is_LF,
        Enum_Structure_Types Structure_Type,
        Enum_Concurrency_Models Concurrency_Model,
        bool Is_Waiting,
        typename T,
        typename Allocator,
        typename Options>
    class Stack {};
} // namespace BA_Concurrency

#endif // STACK_BASE_HPP
