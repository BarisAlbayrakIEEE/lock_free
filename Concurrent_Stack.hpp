#ifndef CONCURRENT_STACK_HPP
#define CONCURRENT_STACK_HPP

#include "enum_structure_types.hpp"
#include "enum_concurrency_models.hpp"

namespace BA_Concurrency {
    template <
        bool Is_LF,
        Enum_Structure_Types Structure_Type,
        Enum_Concurrency_Models Concurrency_Model,
        typename T,
        typename... Args>
    class Concurrent_Stack {};
} // namespace BA_Concurrency

#endif // CONCURRENT_STACK_HPP
