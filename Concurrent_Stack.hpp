// Primary template for Concurrent_Stack.
// Specialized by:
//   - structure type
//   - concurrency model
//   - some additional case dependent arguments
//     Ex: Enum_Structure_Types::Linked requires a memory reclaimer pattern
//         such as the hazard pointers
#ifndef CONCURRENT_STACK_HPP
#define CONCURRENT_STACK_HPP

#include <type_traits>
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
}

#endif // CONCURRENT_STACK_HPP
