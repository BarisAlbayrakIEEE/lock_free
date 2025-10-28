#ifndef QUEUE_LF_LINKED_NODE_HPP
#define QUEUE_LF_LINKED_NODE_HPP

template <class T>
struct queue_LF_linked_node {
    T _data;
    queue_LF_linked_node* _next;
    queue_LF_linked_node(T const& data) : _data(data) {};
};

#endif // QUEUE_LF_LINKED_NODE_HPP
