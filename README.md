**Contents**
- [1. Introduction](#sec1)
- [2. Design Review](#sec2)
  - [2.1. Concurrent_Queue_LF_Ring_MPMC](#sec21)
    - [2.1.1. Description](#sec211)
    - [2.1.2. Requirements](#sec212)
    - [2.1.3. Invariants](#sec213)
    - [2.1.4. Semantics](#sec214)
    - [2.1.5. Progress](#sec215)
    - [2.1.6. Notes](#sec216)
    - [2.1.7. Cautions](#sec217)
    - [2.1.8. TODO](#sec218)
  - [2.2. Concurrent_Stack_LF_Linked_MPSC](#sec22)
    - [2.2.1. Description](#sec221)
    - [2.2.2. Requirements](#sec222)
    - [2.2.3. Invariants](#sec223)
    - [2.2.4. Semantics](#sec224)
    - [2.2.5. Progress](#sec225)
    - [2.2.6. Notes](#sec226)
    - [2.2.7. Cautions](#sec227)
    - [2.2.8. TODO](#sec228)
  - [2.3. Concurrent_Stack_LF_Linked_Hazard_MPMC](#sec23)
    - [2.3.1. Description](#sec231)
    - [2.3.2. Requirements](#sec232)
    - [2.3.3. Invariants](#sec233)
    - [2.3.4. Semantics](#sec234)
    - [2.3.5. Progress](#sec235)
    - [2.3.6. Notes](#sec236)
    - [2.3.7. Cautions](#sec237)
    - [2.3.8. TODO](#sec238)

**PREFACE**\
I created this repository as a reference for my job applications.
The code given in this repository is:
- to introduce my experience with lock-free concurrency, atomic operations and the required C++ utilities,
- to present my background with the concurrent data structures,
- not to provide a tested production-ready multi-platform library.

# 1. Introduction <a id='sec1'></a>
The lock-free concurrency is one of the major topics especially in low-latency architectures.
The two fundamental data structures are studied alot in this respect: queue and stack.

In this repository, I will cover simple designs for the following configurations:
- A ring buffer MPMC lock-free queue with ticket-based synchronization satisfying only the **logical FIFO**,
- A link-based MPSC lock-free stack with a user defined allocator,
- A link-based MPMC lock-free stack with a user defined allocator and hazard pointers for the memory reclamation,
- A link-based MPMC lock-free stack with a user defined allocator and read-copy-update (RCU) reclamation for the memory,
- A link-based MPMC lock-free stack with a user defined allocator and interval-based reclamation (IBR) for the memory.

All designs support the MPMC scenario.

# 2. Design Review <a id='sec2'></a>

## 2.1. Concurrent_Queue_LF_Ring_MPMC <a id='sec21'></a>
A ring buffer lock-free queue with ticket-based synchronization satisfying only the **logical FIFO**.
The design is similar with the following libraries:
- boost::lock_free::queue
- moodycamel::ConcurrentQueue
- liblfds unbounded MPMC queue

### 2.1.1. Description <a id='sec211'></a>
Synchronizes the two atomic tickets, **head** and **tail**,
in order to synchronize the producers and consumers.
The tickets locate the head and tail pointer of the queue
while effectively managing the states of each slot (**FULL** or **EMPTY**).

### 2.1.2. Requirements <a id='sec212'></a>
- T must be noexcept-constructible.
- T must be noexcept-movable.

### 2.1.3. Invariants <a id='sec213'></a>
Producers and consumers shall hold the state invariant.
See the definitions of head and tail members for the tickets
that allow managing the slot states.
The state invariants are as follows:
1. For a **FULL** slot (i.e. contains published data) the following equality shall hold:\
`slot._expected_ticket == producer_ticket`
2. For an **EMPTY** slot (i.e. data is popped successfuly) the following equality shall hold:\
`slot._expected_ticket == consumer_ticket + 1`

### 2.1.4. Semantics <a id='sec214'></a>
**Slot class:**
The ring buffer is a contiguous array of Slot objects.
A slot encapsulates two members:
1. The data is stored in a byte array of size of T.
2. The expected ticket of the slot
which flexibly defines the state of the slot as **FULL** or **EMPTY**.

The threads are completely isolated by the well aligned slots (no false sharing)
such that each thread works on a different slot at any time.
This is provided by the ticket approach.
The producers rely on the shared tail ticket
while the consumers use the head ticket.
Hence, the only contention is on the head and tail tickets
among the producers and consumers respectively.
The only exception is try_pop method
which loads tail for an optimization for the empty-queue edge case.
See the Cautions section below which states that
the single consumer configurations exclude the use of tail ticket
in order to optimize the tail synchronization.

The shared use of the head and tail tickets
disappears for the single producer and single consumer configurations
keeping in mind that these configurations
would exclude tail in try_pop function.
The single producer configurations would replace
the atomic type of the tail ticket with a regular non-atomic type,
while the single consumer configurations would do the same for the head ticket.

**push():**
1. Increment the tail to obtain the producer ticket:\
`const std::size_t producer_ticket = tail.value.fetch_add(1, std::memory_order_acq_rel);`
2. Wait until the slot expects the obtained producer ticket:\
`while (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket);`
3. The slot is owned now. push the data:\
`::new (slot.to_ptr()) T(std::forward<U>(data));`
4. Publish the data by marking it as **FULL** (`expected_ticket = consumer_ticket + 1`).
Notice that, the **FULL** condition will be satisfied
when the consumer ticket reaches the current producer ticket:\
`slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);`

**pop():**
1. Increment the head to obtain the consumer ticket:\
`std::size_t consumer_ticket = head.value.fetch_add(1, std::memory_order_acq_rel);`
2. Wait until the slot expects the obtained consumer ticket:\
`while (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1);`
3. The slot is owned now. pop the data:\
`T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };`
4. If not trivially destructible, call the T's destructor:\
`if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();`
5. Mark the slot as **EMPTY** (`expected_ticket = producer_ticket`).
Notice that, the **EMPTY** condition will be satisfied
when the producer reaches the next round of this consumer ticket:\
`slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);`
6. `return data;`

**try_push():**\
Having an infinite loop at the top to eliminate spurious failure of the weak CAS:
1. Load the tail to the producer ticket:\
`std::size_t producer_ticket = tail.value.load(std::memory_order_acquire);`
2. Inspect if the slot is **FULL** for this producer ticket:\
`if (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket) return false;`
3. Weak CAS the tail to get the ownership of the slot:\
`if (!tail.value.compare_exchange_strong(producer_ticket, producer_ticket + 1,...)) continue;`
4. The slot is owned now, push the data:\
`::new (slot.to_ptr()) T(std::forward<U>(data));`
5. Publish the data by marking it as **FULL** (`expected_ticket = consumer_ticket + 1`).
Notice that, the **FULL** condition will be satisfied
when the consumer ticket reaches the current producer ticket:\
`slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);`
6. return true;

**try_pop():**\
Having an infinite loop at the top to eliminate spurious failure of the weak CAS:
1. Load the head to the consumer ticket:\
`std::size_t consumer_ticket = head.value.load(std::memory_order_acquire);`
2. Inspect if the queue is empty (an optimization for the empty queue):\
`if (consumer_ticket == tail.value.load(std::memory_order_acquire)) return std::nullopt;`
3. Inspect if the slot is **EMPTY** for this consumer ticket:\
`if (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1) return false;`
4. Weak CAS the head to get the ownership of the slot:\
`if (!head.value.compare_exchange_strong(consumer_ticket, consumer_ticket + 1,...)) continue;`
5. The slot is owned now, pop the data:\
`T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };`
6. If not trivially destructible, call the T's destructor:\
`if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();`
7. Mark the slot as **EMPTY** (`expected_ticket = producer_ticket`).
Notice that, the **EMPTY** condition will be satisfied
when the producer reaches the next round of this consumer ticket:\
`slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);`
8. return data;

### 2.1.5. Progress <a id='sec215'></a>
The **queue of liblfds library** is based on **Dmitry Vyukov's** lock-free queue but
is not lock-free as is discussed in this [thread](https://stackoverflow.com/a/54755605).

The liblfds queue blocks the head and tail pointers and the associated threads
until the ticket requirement defined by **FULL** and **EMPTY** rules is satisfied.
While a thread is blocked waiting for its reserved slot,
the other threads are also blocked as the head and tail pointers
are only advanced when the thread achieves to satisfy the ticket condition.
In other words, threads are fully serialized and
only one can execute and the others are blocked.
In summary, the liblfds queue has no lock-freedom (even not obstruct-free).

The push function of the liblfds queue is as follows:

```
producer_ticket = tail.load()
INFINITE LOOP
  IF producer_ticket == slot._expected_ticket.load()
    IF tail.CAS(producer_ticket, producer_ticket + 1)
      BREAK
Push the data
slot._expected_ticket.store(producer_ticket + 1);
```

As explained above, the producers are all blocked until
the slot is ready for push (i.e. **EMPTY**) and the producer succeeds the CAS operation.
This blocking algorithm is choosen to satisfy **the stcict temporal FIFO**.
The producers push one-by-one and the consumers pop one-by-one.

On the other hand, in my design,
the FIFO invariant is relaxed to provide the FIFO order **only logically**
such that the producers and consumers walk through an ordered buffer of slots.
As the temporal FIFO is excluded, the head and tail pointers can be advanced freely.
Hence, the CAS operation used by the queue of liblfds
can safely be replaced by a fetch_add operation:

```
producer_ticket = tail.fetch_add();
while(!slot._expected_ticket.load() != producer_ticket);
Push the data
slot._expected_ticket.store(producer_ticket + 1);
```

This design implies a producer is blocked waiting for the corresponding consumer
to finish popping the data from the slot.
The pop function is symmetric such that
the consumer is blocked waiting for the corresponding producer to finish publishing the data.

The relaxed FIFO ordering is a common way of achieving the lock-freedom.
[moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue.git)
follows a similar approach with a couple of optimization details.

### 2.1.6. Notes <a id='sec216'></a>
1. Memory orders are chosen to
release data before the visibility of the state transitions and
to acquire data after observing the state transitions.
2. push back-pressures when the queue is full by spinning on its reserved slot while
pop back-pressures when the queue is empty by spinning on its reserved slot.
3. This design supports the MPMC configuration and
can be optimized for single producer/consumer configurations: MPSC, SPMC and SPSC.

### 2.1.7. Cautions <a id='sec217'></a>
1. Threads may spin indefinitely if a counterpart thread fails mid-operation,
before setting the expected state accordingly.
2. Use queue_LF_ring_MPMC alias at the end of the [header file](Concurrent_Queue_LF_Ring_MPMC.hpp)
to get the right specialization of Concurrent_Queue and
to achieve the default arguments consistently.
3. As stated in the Progress section, 
this version does not preserve the FIFO order temporally.

### 2.1.8. TODO <a id='sec218'></a>
1. The blocking operations (push and pop) back-pressures
waiting the expected ticket of the reserved slot to satisfy ticket invariants.
An exponential backoff strategy is required for these blocking operations.
2. Similar to the 1st one, the edge cases (empty queue and full queue)
requires an exponential backoff strategy as well.

## 2.2. Concurrent_Stack_LF_Linked_MPSC <a id='sec22'></a>

### 2.2.1. Description <a id='sec221'></a>
In case of multiple consumers, the pop operation needs to reclaim the memory for the head node.
See [MPMC](#sec231) for the details.
Single consumer terminates the need for this synchronization.
In other words, in case of a single consumer,
the hazard pointers (or epochs) are not required.

### 2.2.2. Requirements <a id='sec222'></a>
- T must be noexcept-movable.

### 2.2.3. Invariants <a id='sec223'></a>
Strict LIFO

### 2.2.4. Semantics <a id='sec224'></a>
**push():**\
Follows the classical algorithm for the push:
1. Create a new node.
2. Set the next pointer of the new node to the current head.
3. Apply CAS on the head: CAS(new_node->head, new_node)

**pop():**\
The classical pop routine without a need for the synchronized memory reclamation:
1. Apply CAS on the head: `CAS(head, head->next)`
2. Move the data out from the old head node
3. Add the old head to the reclaim list
4. Return the data

### 2.2.5. Progress <a id='sec225'></a>
Strictly lock-free execution as the threads serializing on the head node
are bound to functions (push and pop) with constant time complexity, O(1).

### 2.2.6. Notes <a id='sec226'></a>
Memory orders are chosen to
release data before the visibility of the state transitions and
to acquire data after observing the state transitions.

### 2.2.7. Cautions <a id='sec227'></a>
1. In case of a single producer (i.e. SPSC),
the competition between the single producer and the consumer remain
which means that the synchronization between the counterparts is still required.
On the other hand, single consumer configuration is special
and is explained in [Concurrent_Stack_LF_Linked_Hazard_MPMC](#sec237).
Correspondingly, the following configurations are the asme: MPSC and SPSC.
2. use stack_LF_Linked_MPSC and stack_LF_Linked_SPSC aliases
at the end of the [header file](Concurrent_Stack_LF_Linked_MPSC.hpp)
to get the right specialization of Concurrent_Stack and
to achieve the default arguments consistently.

### 2.2.8. TODO <a id='sec228'></a>
The exponential backoff for the head node
in order to deal with the high CAS contention on the head.

## 2.3. Concurrent_Stack_LF_Linked_Hazard_MPMC <a id='sec23'></a>

### 2.3.1. Description <a id='sec231'></a>
Pop operation needs to reclaim the memory for the head node.
However, the other consumer threads working on the same head
would have dangling pointers if the memory reclaim is not synchronized.
Hazard pointers solve this issue by protecting the registered object.
The memory can be reclaimed only when there exists no assigned hazard pointer.

### 2.3.2. Requirements <a id='sec232'></a>
- T must be noexcept-movable.

### 2.3.3. Invariants <a id='sec233'></a>
Strict LIFO

### 2.3.4. Semantics <a id='sec234'></a>
**push():**\
Follows the classical algorithm for the push:
1. Create a new node.
2. Set the next pointer of the new node to the current head.
3. Apply CAS on the head: CAS(new_node->head, new_node)

**pop():**\
The classical pop routine is tuned for the memory reclamation under the protection of hazard pointers:
1. Protect the head node by a hazard pointer
2. Apply CAS on the head: `CAS(head, head->next)`
3. Move the data out from the old head node
4. Clear the hazard pointer
5. Add the old head to the reclaim list
6. Return the data

See the documentation of the [hazard pointer header](Hazard_Ptr.hpp)
for the details about the hazard pointers.

### 2.3.5. Progress <a id='sec235'></a>
Strictly lock-free execution as the threads serializing on the head node
are bound to functions (push and pop) with constant time complexity, O(1).

### 2.3.6. Notes <a id='sec236'></a>
1. Memory orders are chosen to
release data before the visibility of the state transitions and
to acquire data after observing the state transitions.

### 2.3.7. Cautions <a id='sec237'></a>
1. In case of a single producer (i.e. SPMC and SPSC),
the competition between the single producer and the consumer(s) remain
which means that the synchronization between the counterparts is still required.
On the other hand, the single consumer configuration is special
and is explained in the next caution.
As the single producer configuration has no effect on this design,
I will use the same specialization for the following two configurations: MPMC and SPMC
2. The hazard pointers synchronize the memory reclamation
(i.e. the race condition related to the head pointer destruction at the end of a pop).
The race condition disappears when there exists a single consumer.
Hence, the usage of hazard pointers is
limited to the SPMC and MPMC configurations and
the repository SPSC and MPSC configurations do not utilize the hazard pointers.
3. use stack_LF_Linked_MPMC and stack_LF_Linked_SPMC aliases
at the end of the [header file](Concurrent_Stack_LF_Linked_Hazard_MPMC.hpp)
to get the right specialization of Concurrent_Stack and
to achieve the default arguments consistently.

### 2.3.8. TODO <a id='sec238'></a>
1. Consider exponential backoff for the head node in order to deal with the high CAS contention on the head.
