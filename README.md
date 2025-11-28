**Contents**
- [1. Introduction](#sec1)
- [2. Design Review](#sec2)
  - [2.1. Concurrent_Queue__Blocking](#sec201)
    - [2.1.1. Description](#sec2011)
    - [2.1.2. Requirements](#sec2012)
    - [2.1.3. Invariants](#sec2013)
    - [2.1.4. Semantics](#sec2014)
    - [2.1.5. Progress](#sec2015)
    - [2.1.6. Notes](#sec2016)
    - [2.1.7. Cautions](#sec2017)
    - [2.1.8. TODO](#sec2018)
  - [2.2. Concurrent_Queue__LF_Ring_MPMC](#sec202)
    - [2.2.1. Description](#sec2021)
    - [2.2.2. Requirements](#sec2022)
    - [2.2.3. Invariants](#sec2023)
    - [2.2.4. Semantics](#sec2024)
    - [2.2.5. Progress](#sec2025)
    - [2.2.6. Notes](#sec2026)
    - [2.2.7. Cautions](#sec2027)
    - [2.2.8. TODO](#sec2028)
  - [2.3. Concurrent_Queue__LF_Ring_MPSC](#sec203)
    - [2.3.1. Description](#sec2031)
    - [2.3.2. Requirements](#sec2032)
    - [2.3.3. Invariants](#sec2033)
    - [2.3.4. Semantics](#sec2034)
    - [2.3.5. Progress](#sec2035)
    - [2.3.6. Notes](#sec2036)
    - [2.3.7. Cautions](#sec2037)
    - [2.3.8. TODO](#sec2038)
  - [2.4. Concurrent_Stack__LF_Linked_MPSC](#sec204)
    - [2.4.1. Description](#sec2041)
    - [2.4.2. Requirements](#sec2042)
    - [2.4.5. Invariants](#sec2043)
    - [2.4.4. Semantics](#sec2044)
    - [2.4.5. Progress](#sec2045)
    - [2.4.6. Notes](#sec2046)
    - [2.4.7. Cautions](#sec2047)
    - [2.4.8. TODO](#sec2048)
  - [2.5. Concurrent_Stack__LF_Linked_Hazard_MPMC](#sec205)
    - [2.5.1. Description](#sec2051)
    - [2.5.2. Requirements](#sec2052)
    - [2.5.3. Invariants](#sec2053)
    - [2.5.4. Semantics](#sec2054)
    - [2.5.5. Progress](#sec2055)
    - [2.5.6. Notes](#sec2056)
    - [2.5.7. Cautions](#sec2057)
    - [2.5.8. TODO](#sec2058)
  - [2.6. Thread_Pool__Blocking](#sec206)
    - [2.6.1. Description](#sec2061)
    - [2.6.2. Requirements](#sec2062)
    - [2.6.3. Invariants](#sec2063)
    - [2.6.4. Semantics](#sec2064)
    - [2.6.5. Progress](#sec2065)
    - [2.6.6. Notes](#sec2066)
    - [2.6.7. Cautions](#sec2067)
    - [2.6.8. TODO](#sec2068)
  - [2.7. Thread_Pool__Deadline](#sec207)
    - [2.7.1. Description](#sec2071)
    - [2.7.2. Requirements](#sec2072)
    - [2.7.3. Invariants](#sec2073)
    - [2.7.4. Semantics](#sec2074)
    - [2.7.5. Progress](#sec2075)
    - [2.7.6. Notes](#sec2076)
    - [2.7.7. Cautions](#sec2077)
    - [2.7.8. TODO](#sec2078)
  - [2.8. Thread_Pool__Work_Stealing](#sec208)
    - [2.8.1. Description](#sec2081)
    - [2.8.2. Requirements](#sec2082)
    - [2.8.3. Invariants](#sec2083)
    - [2.8.4. Semantics](#sec2084)
    - [2.8.5. Progress](#sec2085)
    - [2.8.6. Notes](#sec2086)
    - [2.8.7. Cautions](#sec2087)
    - [2.8.8. TODO](#sec2088)
  - [2.9. Thread_Pool__LF](#sec209)
    - [2.9.1. Description](#sec2091)
    - [2.9.2. Requirements](#sec2092)
    - [2.9.3. Invariants](#sec2093)
    - [2.9.4. Semantics](#sec2094)
    - [2.9.5. Progress](#sec2095)
    - [2.9.6. Notes](#sec2096)
    - [2.9.7. Cautions](#sec2097)
    - [2.9.8. TODO](#sec2098)
  - [2.10. Thread_Pool__NUMA_Aware](#sec210)
    - [2.10.1. Description](#sec2101)
    - [2.10.2. Requirements](#sec2102)
    - [2.10.3. Invariants](#sec2103)
    - [2.10.4. Semantics](#sec2104)
    - [2.10.5. Progress](#sec2105)
    - [2.10.6. Notes](#sec2106)
    - [2.10.7. Cautions](#sec2107)
    - [2.10.8. TODO](#sec2108)
  - [2.11. Thread_Pool__Actor](#sec211)
    - [2.11.1. Description](#sec2111)
    - [2.11.2. Requirements](#sec2112)
    - [2.11.3. Invariants](#sec2113)
    - [2.11.4. Semantics](#sec2114)
    - [2.11.5. Progress](#sec2115)
    - [2.11.6. Notes](#sec2116)
    - [2.11.7. Cautions](#sec2117)
    - [2.11.8. TODO](#sec2118)

**PREFACE**\
I created this repository as a reference for my job applications.
The code given in this repository is:
- to introduce my experience with lock-free concurrency, atomic operations and the required C++ utilities,
- to present my background with the concurrent data structures,
- not to provide a tested production-ready multi-platform library.

# 1. Introduction <a id='sec1'></a>
The lock-free concurrency is one of the major topics especially in low-latency architectures.
The two fundamental data structures are studied a lot in this respect: queue and stack.

In terms of memory layout, the two data structures can use a contiguous buffer or store the elements dynamically.
In terms of a lock-free design, a stack cannot be designed on top of a ring buffer due to the LIFO requirements.
Hence, I will use a linked list while designing the stack data structure.
I will use the ring buffer for the queue as it is very efficient for the FIFO ordering.

In this repository, I will cover simple designs for the following configurations:
- A very simple link-based lock-based blocking queue,
- A ring buffer MPMC lock-free queue with ticket-based synchronization satisfying only the **logical FIFO**,
- A ring buffer MPSC lock-free queue with ticket-based synchronization satisfying only the **logical FIFO**,
- A link-based MPSC lock-free stack with a user defined allocator,
- A link-based MPMC lock-free stack with a user defined allocator and hazard pointers for the memory reclamation,
- A link-based MPMC lock-free stack with a user defined allocator and read-copy-update (RCU) reclamation for the memory,
- A link-based MPMC lock-free stack with a user defined allocator and interval-based reclamation (IBR) for the memory.

The repository additionally contains simple designs for the well-known thread pools:
- blocking,
- work-stealing,
- deadline,
- lock-free (under construction),
- NUMA-aware (under construction).
- actor (under construction and buggy),

The repository additionally contains simple designs for the followings:
- A few type traits,
- A wrapper class to fit objects to a cache line,
- A simple STL style arena working on the static memory,
- Hazard pointer utilities.

# 2. Design Review <a id='sec2'></a>

## 2.1. Concurrent_Queue__Blocking <a id='sec201'></a>
Although this is a lock-free library, I included some simple designs for the blocking lock-based architectures only for demonstration.
This is a wrapper class around a standard queue allowing synchronized concurrent access via the mutual exclusion.

### 2.1.1. Description <a id='sec2011'></a>
TODO

### 2.1.2. Requirements <a id='sec2012'></a>
TODO

### 2.1.3. Invariants <a id='sec2013'></a>
TODO

### 2.1.4. Semantics <a id='sec2014'></a>
TODO

### 2.1.5. Progress <a id='sec2015'></a>
TODO

### 2.1.6. Notes <a id='sec2016'></a>
TODO

### 2.1.7. Cautions <a id='sec2017'></a>
TODO

### 2.1.8. TODO <a id='sec2018'></a>
TODO

## 2.2. Concurrent_Queue__LF_Ring_MPMC <a id='sec202'></a>
This is a ring buffer lock-free queue with ticket-based synchronization satisfying only the **logical FIFO**.
The design is similar with the following libraries:
- boost::lock_free::queue
- moodycamel::ConcurrentQueue
- liblfds unbounded MPMC queue

### 2.2.1. Description <a id='sec2021'></a>
Synchronizes the two atomic tickets, **head** and **tail**, in order to synchronize the producers and consumers.
The tickets locate the head and tail pointer of the queue while effectively managing the states of each slot (**FULL** or **EMPTY**).

### 2.2.2. Requirements <a id='sec2022'></a>
- T must be noexcept-constructible.
- T must be noexcept-movable.

### 2.2.3. Invariants <a id='sec2023'></a>
This is a stateless queue design but acts like a stateful data structure where the producers and consumers shall hold the state invariant.
The state invariants are as follows:
1. For a **FULL** slot (i.e. contains published data) the following equality shall hold:\
`slot._expected_ticket == producer_ticket`
2. For an **EMPTY** slot (i.e. data is popped successfuly) the following equality shall hold:\
`slot._expected_ticket == consumer_ticket + 1`

The enqueue and dequeue functions are based on these two invariants.

### 2.2.4. Semantics <a id='sec2024'></a>
**Slots**\
The ring buffer is a contiguous array of Slot objects.
A slot encapsulates two members:
1. The data is stored in a byte array of size of T.
2. The expected ticket of the slot
which flexibly defines the state of the slot as **FULL** or **EMPTY**.

The use of byte array as a storage allows multiple threads to perform construction and destruction on the buffer concurrently.

The threads are completely isolated by the well aligned slots (no false sharing) such that each thread works on a different slot at any time.
This is provided by the ticket approach.
The producers rely on the shared tail ticket while the consumers use the head ticket.
Hence, the only contention is on the head and tail tickets among the producers and consumers respectively.
The only exception is try_pop method which loads tail for an optimization for the empty-queue edge case.
In order to optimize the tail synchronization, the single consumer configurations exclude the use of tail ticket in try_pop function.

The shared use of the head and tail tickets disappears for the single producer and single consumer configurations keeping in mind that these configurations would exclude tail in try_pop function.
The single producer configurations would replace the atomic type of the tail ticket with a regular non-atomic type, while the single consumer configurations would do the same for the head ticket.

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

### 2.2.5. Progress <a id='sec2025'></a>
The **queue of liblfds library** is based on **Dmitry Vyukov's** lock-free queue but is not lock-free as discussed in this [thread](https://stackoverflow.com/a/54755605).

The liblfds queue blocks the head and tail pointers and the associated threads until the ticket requirement defined by **FULL** and **EMPTY** rules is satisfied.
While a thread is blocked waiting for its reserved slot, the other threads are also blocked as the head and tail pointers are only advanced when the thread achieves to satisfy the ticket condition.
In other words, threads are fully serialized and only one can execute and the others are blocked.
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

As explained above, the producers are all blocked until the slot is ready for push (i.e. **EMPTY**) and the producer succeeds the CAS operation.
This blocking algorithm is choosen to satisfy **the stcict temporal FIFO**.
The producers push one-by-one and the consumers pop one-by-one.

On the other hand, in my design, the FIFO invariant is relaxed to provide the FIFO order **only logically** such that the producers and consumers walk through an ordered buffer of slots.
As the temporal FIFO is excluded, the head and tail pointers can be advanced freely.
Hence, the CAS operation used by the queue of liblfds can safely be replaced by a fetch_add operation:

```
producer_ticket = tail.fetch_add();
while(!slot._expected_ticket.load() != producer_ticket);
Push the data
slot._expected_ticket.store(producer_ticket + 1);
```

This design implies a producer is blocked waiting for the corresponding consumer to finish popping the data from the slot.
The pop function is symmetric such that the consumer is blocked waiting for the corresponding producer to finish publishing the data.

The relaxed FIFO ordering is a common way of achieving the lock-freedom.
[moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue.git) follows a similar approach with a couple of optimization details.

### 2.2.6. Notes <a id='sec2026'></a>
1. Memory orders are chosen to release data before the visibility of the state transitions and to acquire data after observing the state transitions.
2. push back-pressures when the queue is full by spinning on its reserved slot while pop back-pressures when the queue is empty by spinning on its reserved slot.
3. This design supports the MPMC configuration and can be optimized for single producer/consumer configurations: MPSC, SPMC and SPSC.

### 2.2.7. Cautions <a id='sec2027'></a>
1. Threads may spin indefinitely if a counterpart thread fails mid-operation,
before setting the expected state accordingly.
2. Use queue_LF_ring_MPMC alias at the end of the [header file](Concurrent_Queue__LF_Ring_MPMC.hpp)
to get the right specialization of Concurrent_Queue and to achieve the default arguments consistently.
3. As stated in [Progress](#sec2025), this version does not preserve the FIFO order temporally.

### 2.2.8. TODO <a id='sec2028'></a>
1. The blocking operations (push and pop) back-pressures waiting the expected ticket of the reserved slot to satisfy ticket invariants.
An exponential backoff strategy is required for these blocking operations.
2. Similar to the 1st one, the edge cases (empty queue and full queue) requires an exponential backoff strategy as well.

## 2.3. Concurrent_Queue__LF_Ring_MPSC <a id='sec203'></a>
This is a specialization of the MPMC case for the single consumer configuration.

### 2.3.1. Description <a id='sec2031'></a>
TODO

### 2.3.2. Requirements <a id='sec2032'></a>
TODO

### 2.3.3. Invariants <a id='sec2033'></a>
TODO

### 2.3.4. Semantics <a id='sec2034'></a>
TODO

### 2.3.5. Progress <a id='sec2035'></a>
TODO

### 2.3.6. Notes <a id='sec2036'></a>
TODO

### 2.3.7. Cautions <a id='sec2037'></a>
TODO

### 2.3.8. TODO <a id='sec2038'></a>
TODO

## 2.4. Concurrent_Stack__LF_Linked_MPSC <a id='sec204'></a>

### 2.4.1. Description <a id='sec2041'></a>
In case of multiple consumers, the pop operation needs to reclaim the memory for the head node which must be synchronized for the consumer threads.
See [MPMC](#sec2051) for the details.
Single consumer configuration does not need this synchronization at all.
In other words, in case of a single consumer, the hazard pointers (or epochs) are not required.

### 2.4.2. Requirements <a id='sec2042'></a>
- T must be noexcept-movable.

### 2.4.5. Invariants <a id='sec2043'></a>
Strict LIFO

### 2.4.4. Semantics <a id='sec2044'></a>
**push():**\
Follows the classical algorithm for the push:
1. Create a new node.
2. Set the next pointer of the new node to the current head.
3. Apply CAS on the head: `CAS(new_node->head, new_node)`

**pop():**\
The classical pop routine without a need for the synchronized memory reclamation:
1. Apply CAS on the head: `CAS(head, head->next)`
2. Move the data out from the old head node
3. Delete the old head
4. Return the data

### 2.4.5. Progress <a id='sec2045'></a>
Strict lock-free execution as the threads serializing on the head node are bound to functions (push and pop) with constant time complexity, O(1).

### 2.4.6. Notes <a id='sec2046'></a>
Memory orders are chosen to release data before the visibility of the state transitions and to acquire data after observing the state transitions.

### 2.4.7. Cautions <a id='sec2047'></a>
1. In case of a single producer (i.e. SPSC), the competition between the single producer and the consumer remain which means that the synchronization between the counterparts is still required.
On the other hand, single consumer configuration is special and is explained in [Concurrent_Stack__LF_Linked_Hazard_MPMC](#sec2057).
Correspondingly, the following configurations are the asme: MPSC and SPSC.
2. use stack_LF_Linked_MPSC and stack_LF_Linked_SPSC aliases at the end of the [header file](Concurrent_Stack__LF_Linked_MPSC.hpp) to get the right specialization of Concurrent_Stack and to achieve the default arguments consistently.

### 2.4.8. TODO <a id='sec2048'></a>
The exponential backoff for the head node in order to deal with the high CAS contention on the head.

## 2.5. Concurrent_Stack__LF_Linked_Hazard_MPMC <a id='sec205'></a>

### 2.5.1. Description <a id='sec2051'></a>
Pop operation needs to reclaim the memory for the head node.
However, the other consumer threads working on the same head would have dangling pointers if the memory reclaim is not synchronized.
Hazard pointers solve this issue by protecting the registered object.
The memory can be reclaimed only when there exists no assigned hazard pointer.

### 2.5.2. Requirements <a id='sec2052'></a>
- T must be noexcept-movable.

### 2.5.3. Invariants <a id='sec2053'></a>
Strict LIFO

### 2.5.4. Semantics <a id='sec2054'></a>
**push():**\
Follows the classical algorithm for the push:
1. Create a new node.
2. Set the next pointer of the new node to the current head.
3. Apply CAS on the head: `CAS(new_node->head, new_node)`

**pop():**\
The classical pop routine is tuned for the memory reclamation under the protection of hazard pointers:
1. Protect the head node by a hazard pointer
2. Apply CAS on the head: `CAS(head, head->next)`
3. Move the data out from the old head node
4. Clear the hazard pointer
5. Add the old head to the reclaim list
6. Return the data

See the documentation of the [hazard pointer header](Hazard_Ptr.hpp) for the details about the hazard pointers.

### 2.5.5. Progress <a id='sec2055'></a>
Strict lock-free execution as the threads serializing on the head node are bound to functions (push and pop) with constant time complexity, O(1).

### 2.5.6. Notes <a id='sec2056'></a>
Memory orders are chosen to release data before the visibility of the state transitions and to acquire data after observing the state transitions.

### 2.5.7. Cautions <a id='sec2057'></a>
1. In case of a single producer (i.e. SPMC and SPSC), the competition between the single producer and the consumer(s) remain which means that the synchronization between the counterparts is still required.
On the other hand, the single consumer configuration is special and is explained in the next caution.
As the single producer configuration has no effect on this design,
I will use the same specialization for the following two configurations: MPMC and SPMC
2. use stack_LF_Linked_MPMC and stack_LF_Linked_SPMC aliases at the end of the [header file](Concurrent_Stack__LF_Linked_Hazard_MPMC.hpp) to get the right specialization of Concurrent_Stack and to achieve the default arguments consistently.

### 2.5.8. TODO <a id='sec2058'></a>
Consider exponential backoff for the head node in order to deal with the high CAS contention on the head.

## 2.6. Thread_Pool__Blocking <a id='sec206'></a>
TODO

### 2.6.1. Description <a id='sec2061'></a>
TODO

### 2.6.2. Requirements <a id='sec2062'></a>
TODO

### 2.6.3. Invariants <a id='sec2063'></a>
TODO

### 2.6.4. Semantics <a id='sec2064'></a>
TODO

### 2.6.5. Progress <a id='sec2065'></a>
TODO

### 2.6.6. Notes <a id='sec2066'></a>
TODO

### 2.6.7. Cautions <a id='sec2067'></a>
TODO

### 2.6.8. TODO <a id='sec2068'></a>
TODO

## 2.7. Thread_Pool__Deadline <a id='sec207'></a>
TODO

### 2.7.1. Description <a id='sec2071'></a>
TODO

### 2.7.2. Requirements <a id='sec2072'></a>
TODO

### 2.7.3. Invariants <a id='sec2073'></a>
TODO

### 2.7.4. Semantics <a id='sec2074'></a>
TODO

### 2.7.5. Progress <a id='sec2075'></a>
TODO

### 2.7.6. Notes <a id='sec2076'></a>
TODO

### 2.7.7. Cautions <a id='sec2077'></a>
TODO

### 2.7.8. TODO <a id='sec2078'></a>
TODO

## 2.8. Thread_Pool__Work_Stealing <a id='sec208'></a>
TODO

### 2.8.1. Description <a id='sec2081'></a>
TODO

### 2.8.2. Requirements <a id='sec2082'></a>
TODO

### 2.8.3. Invariants <a id='sec2083'></a>
TODO

### 2.8.4. Semantics <a id='sec2084'></a>
TODO

### 2.8.5. Progress <a id='sec2085'></a>
TODO

### 2.8.6. Notes <a id='sec2086'></a>
TODO

### 2.8.7. Cautions <a id='sec2087'></a>
TODO

### 2.8.8. TODO <a id='sec2088'></a>
TODO

## 2.9. Thread_Pool__LF (under construction) <a id='sec209'></a>
TODO

### 2.9.1. Description <a id='sec2091'></a>
TODO

### 2.9.2. Requirements <a id='sec2092'></a>
TODO

### 2.9.3. Invariants <a id='sec2093'></a>
TODO

### 2.9.4. Semantics <a id='sec2094'></a>
TODO

### 2.9.5. Progress <a id='sec2095'></a>
TODO

### 2.9.6. Notes <a id='sec2096'></a>
TODO

### 2.9.7. Cautions <a id='sec2097'></a>
TODO

### 2.9.8. TODO <a id='sec2098'></a>
TODO

## 2.10. Thread_Pool__NUMA_Aware (under construction) <a id='sec210'></a>
TODO

### 2.10.1. Description <a id='sec2101'></a>
TODO

### 2.10.2. Requirements <a id='sec2102'></a>
TODO

### 2.10.3. Invariants <a id='sec2103'></a>
TODO

### 2.10.4. Semantics <a id='sec2104'></a>
TODO

### 2.10.5. Progress <a id='sec2105'></a>
TODO

### 2.10.6. Notes <a id='sec2106'></a>
TODO

### 2.10.7. Cautions <a id='sec2107'></a>
TODO

### 2.10.8. TODO <a id='sec2108'></a>
TODO

## 2.11. Thread_Pool__Actor (under construction and buggy) <a id='sec211'></a>
TODO

### 2.11.1. Description <a id='sec2111'></a>
TODO

### 2.11.2. Requirements <a id='sec2112'></a>
TODO

### 2.11.3. Invariants <a id='sec2113'></a>
TODO

### 2.11.4. Semantics <a id='sec2114'></a>
TODO

### 2.11.5. Progress <a id='sec2115'></a>
TODO

### 2.11.6. Notes <a id='sec2116'></a>
TODO

### 2.11.7. Cautions <a id='sec2117'></a>
TODO

### 2.11.8. TODO <a id='sec2118'></a>
TODO
