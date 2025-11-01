#ifndef NODE_HPP
#define NODE_HPP

namespace BA_Concurrency {
    template <typename T>
    struct Node {
        T _data;
        Node* _next{};
        explicit Node(T&& data) : _data(std::move(data)) {}
        explicit Node(const T& data) : _data(data) {}
    };
}

#endif // NODE_HPP
