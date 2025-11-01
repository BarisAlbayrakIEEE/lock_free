#ifndef NODE_HPP
#define NODE_HPP

#include <utility>
#include <type_traits>

namespace BA_Concurrency {
    template <typename T>
    struct Node {
        T _data;
        Node* _next{};
        explicit Node(T&& data) noexcept(std::is_nothrow_move_constructible_v<T>)
            : _data(std::move(data)) {};
        explicit Node(const T& data) : _data(data) {};
        ~Node() = default;
    };
}

#endif // NODE_HPP
