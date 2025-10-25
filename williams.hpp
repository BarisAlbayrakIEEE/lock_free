#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>
#include <type_traits>
#include <variant>
#include <memory>
#include <functional>












template<typename T>
class lock_free_stack__assume_lock_free_shared_ptr {
private:
    struct Node {
        std::shared_ptr<T> _data;
        Node* _next;
        Node(T const& data) : _data(std::make_shared<T>(data)) {}
    };
    std::shared_ptr<Node> _head;

public:

    void push(T const& data) {
        std::shared_ptr<Node> const new_node = std::make_shared<Node>(data);
        new_node->_next = std::atomic_load(&_head);
        while(!head.compare_exchange_weak(new_node->_next, new_node));
    }
    std::shared_ptr<T> pop() {
        std::shared_ptr<Node> old_head = std::atomic_load(&_head);
        while(
            old_head &&
            !_head.compare_exchange_weak(old_head, old_head->_next.load()));
        if(old_head) {
            std::atomic_store(&old_head->_next, std::shared_ptr<Node>());
            return old_head->_data;
        }
        return std::shared_ptr<T>();
    }
    ~lock_free_stack() {
        while(pop());
    }
};










template<typename T>
class lock_free_stack__count_popping_threads {
private:
    struct Node {
        std::shared_ptr<T> _data;
        Node* _next;
        Node(T const& data) : _data(std::make_shared<T>(data)) {}
    };
    std::atomic<Node*> _head;

    std::atomic<std::size_t> threads_in_pop;
    std::atomic<Node*> to_be_deleted;
    static void delete_nodes(Node* nodes) {
        while(nodes) {
            Node* next = nodes->_next;
            delete nodes;
            nodes = next;
        }
    }
    void try_reclaim(Node* old_head) {
        if(threads_in_pop ==  1) {
            Node* nodes_to_delete = to_be_deleted.exchange(nullptr);
            if(!--threads_in_pop) delete_nodes(nodes_to_delete);
            else if(nodes_to_delete) chain_pending_nodes(nodes_to_delete);
            delete old_head;
        } else {
            chain_pending_node(old_head);
            --threads_in_pop;
        }
    }
    void chain_pending_nodes(Node* nodes) {
        Node* last = nodes;
        while(Node* const next = last->_next) last = next;
        chain_pending_nodes(nodes, last);
    }
    void chain_pending_nodes(Node* first, Node* last) {
        last->_next = to_be_deleted;
        while(
            !to_be_deleted.compare_exchange_weak(
                last->_next, 
                first));
    }
    void chain_pending_node(Node* n) {
        chain_pending_nodes(n, n);
    }

public:

    void push(T const& data) {
        Node* const new_node = new Node(data);
        new_node->_next = _head.load();
        while(!_head.compare_exchange_weak(new_node->_next, new_node));
    }
    
    std::shared_ptr<T> pop() {
        ++threads_in_pop;
        Node* old_head = _head.load();
        while(
            old_head &&
            !_head.compare_exchange_weak(old_head, old_head->_next));
        std::shared_ptr<T> res;
        if(old_head) res.swap(old_head->_data);
        try_reclaim(old_head);
        return res;
    }
};






























struct Hazard_Ptr {
    std::atomic<std::thread::id> _thread_id;
    std::atomic<void*> _ptr;
};

unsigned const MAX_HAZARD_PTRS = 100;
Hazard_Ptr HAZARD_PTRS[MAX_HAZARD_PTRS];

class Hazard_Owner {
    Hazard_Ptr* _hazard_ptr;
public:
    Hazard_Owner(Hazard_Owner const&) = delete;
    Hazard_Owner operator = (Hazard_Owner const&) = delete;
    Hazard_Owner() : _hazard_ptr(nullptr) {
        for(unsigned i = 0; i < MAX_HAZARD_PTRS; ++i) {
            std::thread::id old_id;
            if(
                HAZARD_PTRS[i]._thread_id.compare_exchange_strong(
                    old_id, 
                    std::this_thread::get_id()))
            {
                _hazard_ptr = &HAZARD_PTRS[i];
                break;
            }
        }
        if(!_hazard_ptr) {
            throw std::runtime_error("No hazard pointers available");
        }
    }
    std::atomic<void*>& get_ptr() { return _hazard_ptr->_ptr; }
    ~Hazard_Owner() {
        _hazard_ptr->_ptr.store(nullptr);
        _hazard_ptr->_thread_id.store(std::thread::id());
    }
};

std::atomic<void*>& get_ptr_for_current_thread() {
    thread_local static Hazard_Owner hazard_owner;
    return hazard_owner.get_ptr();
}

bool is_hazard_ptr_assigned_to(void* ptr) {
    for(unsigned i = 0; i < MAX_HAZARD_PTRS; ++i) {
        if(HAZARD_PTRS[i]._ptr.load() ==    ptr) return true;
    }
    return false;
}

template<typename T>
void default_deleter(void* ptr) {
    delete static_cast<T*>(ptr);
}

struct Reclaimable {
    void* _data;
    std::function<void(void*)> _deleter;
    Reclaimable* _next;

    template<typename T>
    explicit Reclaimable(T* ptr)
        :
        _data(ptr), 
        _deleter(&default_deleter<T>), 
        _next(0) {}
    template<typename T>
    explicit Reclaimable(T* ptr, std::function<void(void*)> deleter)
        :
        _data(ptr), 
        _deleter(deleter), 
        _next(0) {}
    ~Reclaimable() { _deleter(_data);}
};

std::atomic<Reclaimable*> RECLAIMABLES;
void add_to_reclaimables(Reclaimable* reclaimable) {
    reclaimable->_next = RECLAIMABLES.load();
    while(!RECLAIMABLES.compare_exchange_weak(reclaimable->_next, reclaimable));
}

void delete_reclaimables_with_no_hazard_ptrs() {
    Reclaimable* current = RECLAIMABLES.exchange(nullptr);
    while(current) {
        Reclaimable* const next = current->_next;
        if(!is_hazard_ptr_assigned_to(current->_data)) delete current;
        else add_to_reclaimables(current);
        current = next;
    }
}

template<typename T>
void reclaim_later(T* data) {
    add_to_reclaimables(new Reclaimable(data));
}

template<typename T>
class lock_free_stack__hazard_ptr {
private:
    struct Node {
        std::shared_ptr<T> _data;
        Node* _next;
        Node(T const& data) : _data(std::make_shared<T>(data)) {}
    };
    std::atomic<Node*> _head;

public:

    void push(T const& data) {
        Node* const new_node = new Node(data);
        new_node->_next = _head.load();
        while(!_head.compare_exchange_weak(new_node->_next, new_node));
    }
    
    std::shared_ptr<T> pop() {
        std::atomic<void*>& ptr = get_ptr_for_current_thread();
        Node* old_head = head.load();
        do {
            Node* temp;
            do {
                temp = old_head;
                ptr.store(old_head);
                old_head = head.load();
            } while(old_head != temp);
        } while(
            old_head &&
            !head.compare_exchange_strong(old_head, old_head->_next));
        
        ptr.store(nullptr);
        std::shared_ptr<T> res;
        if(old_head) {
            res.swap(old_head->_data);
            if(is_hazard_ptr_assigned_to(old_head)) reclaim_later(old_head);
            else delete old_head;
            delete_reclaimables_with_no_hazard_ptrs();
        }
        return res;
    }
};
