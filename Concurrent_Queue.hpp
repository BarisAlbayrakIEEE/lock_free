#ifndef CONCURRENT_QUEUE_HPP
#define CONCURRENT_QUEUE_HPP

#include "enum_structure_types.hpp"
#include "enum_concurrency_models.hpp"

namespace BA_Concurrency {
    template <
        bool Is_LF,
        Enum_Structure_Types Structure_Type,
        Enum_Concurrency_Models Concurrency_Model,
        typename T,
        typename... Args>
    class Concurrent_Queue {};
}

#endif // CONCURRENT_QUEUE_HPP
