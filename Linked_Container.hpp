#ifndef LINKED_CONTAINER_HPP
#define LINKED_CONTAINER_HPP

#include <cstddef>
#include <type_traits>
#include <variant>
#include <memory>
#include "type_traits.hpp"

namespace BA_Concurrency {
    template <typename T>
    struct Node {
        T _data;
        Node* _next{};
        explicit Node(T&& data) : _data(std::move(data)) {}
        explicit Node(const T& data) : _data(data) {}
    };
} // namespace BA_Concurrency

#endif // LINKED_CONTAINER_HPP
