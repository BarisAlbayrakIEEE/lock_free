**Contents**
- [1. Introduction](#sec1)
- [2. Fundamentals of the Software Engineering](#sec2)
  - [2.1. Software Architecture & Design](#sec21)
  - [2.2. Abstraction, Encapsulation and Polymorphism](#sec22)
  - [2.3. Programming Paradigms](#sec23)
  - [2.4. Data Structures and Algorithms](#sec24)
  - [2.5. OOP](#sec25)
  - [2.6. Functional Programming (FP)](#sec26)
  - [2.7. Data Oriented Design (DOD)](#sec27)
  - [2.8. Template Metaprogramming](#sec28)
  - [2.9. Concurrency](#sec29)
- [3. Languages & Tools](#sec3)
- [4. C/C++ Skills](#sec4)
- [5. Repositories](#sec5)
- [6. Some References](#sec6)
- [7. Current Studies](#sec7)

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
- A ring buffer lock-free queue with ticket-based synchronization satisfying only the **logical FIFO**,
- A link-based lock-free stack with a user defined allocator and hazard pointers for the memory reclamation,
- A link-based lock-free stack with a user defined allocator and read-copy-update (RCU) reclamation for the memory,
- A link-based lock-free stack with a user defined allocator and interval-based reclamation (IBR) for the memory.

All designs support the MPMC scenario.

# 2. Design Review <a id='sec2'></a>

## 2.1. Concurrent_Queue_LF_Ring_MPMC <a id='sec21'></a>
A ring buffer lock-free queue with ticket-based synchronization satisfying only the **logical FIFO**.
The design is similar with the following libraries:
- boost::lock_free::queue
- moodycamel::ConcurrentQueue
- liblfds unbounded MPMC queue

### 2.1.1. Description <a id='sec211'></a>
Synchronizes the two atomic tickets, _head and _tail,
in order to synchronize the producers and consumers.
The tickets locate the _head and _tail pointer of the queue
while effectively managing the states of each slot (**FULL** or **EMPTY**).

### 2.1.2. Requirements <a id='sec212'></a>
- T must be noexcept-constructible.
- T must be noexcept-movable.

### 2.1.3. Invariants <a id='sec213'></a>
Producers and consumers shall hold the state invariant.
See the definitions of _head and _tail members for the tickets
that allow managing the slot states.
The state invariants are as follows:
1. For a **FULL** slot (i.e. contains published data) the following equality shall hold:\
`slot._expected_ticket == producer_ticket`
2. For an **EMPTY** slot (i.e. does not contain data) the following equality shall hold:\
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
The producers rely on the shared _tail ticket
while the consumers use the _head ticket.
Hence, the only contention is on the _head and _tail tickets
among the producers and consumers respectively.
The producer threads serialize at _tail while
the consumers serialize at _head.
The only exception is try_pop method
which loads _tail for an optimization for the empty-queue edge case.
See the Cautions section below which states that
the single consumer configurations exclude the use of _tail ticket
in order to optimize the _tail synchronization.

The shared use of the _head and _tail tickets
disappears for the single producer and single consumer configurations
keeping in mind that these configurations
will exclude _tail in try_pop function.
The single producer configurations will replace
the atomic type of the _tail ticket with a regular non-atomic type,
while the single consumer configurations will do the same for the _head ticket.
There exist other issues for the optimization which are discussed
in the documentation of the corresponding header file.

**push():**
1. Increment the _tail to obtain the producer ticket:\
`const std::size_t producer_ticket = _tail.value.fetch_add(1, std::memory_order_acq_rel);`
2. Wait until the slot expects the obtained producer ticket:\
`while (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket);`
3. The slot is owned now. push the data:\
`::new (slot.to_ptr()) T(std::forward<U>(data));`
4. Publish the data by marking it as **FULL** (`expected_ticket = consumer_ticket + 1`).
Notice that, the **FULL** condition will be satisfied
when the consumer ticket reaches the current producer ticket:\
`slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);`

**pop():**
1. Increment the _head to obtain the consumer ticket:\
`std::size_t consumer_ticket = _head.value.fetch_add(1, std::memory_order_acq_rel);`
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
1. Load the _tail to the producer ticket:\
`std::size_t producer_ticket = _tail.value.load(std::memory_order_acquire);`
2. Inspect if the slot is **FULL** for this producer ticket:\
`if (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket) return false;`
3. Weak CAS the _tail to get the ownership of the slot:\
`if (!_tail.value.compare_exchange_strong(producer_ticket, producer_ticket + 1,...)) continue;`
4. The slot is owned now, push the data:\
`::new (slot.to_ptr()) T(std::forward<U>(data));`
5. Publish the data by marking it as **FULL** (`expected_ticket = consumer_ticket + 1`).
Notice that, the **FULL** condition will be satisfied
when the consumer ticket reaches the current producer ticket:\
`slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);`
6. return true;

**try_pop():**\
Having an infinite loop at the top to eliminate spurious failure of the weak CAS:
1. Load the _head to the consumer ticket:\
`std::size_t consumer_ticket = _head.value.load(std::memory_order_acquire);`
2. Inspect if the queue is empty (an optimization for the empty case):\
`if (consumer_ticket == _tail.value.load(std::memory_order_acquire)) return std::nullopt;`
3. Inspect if the slot is **EMPTY** for this consumer ticket:\
`if (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1) return false;`
4. Weak CAS the _head to get the ownership of the slot:\
`if (!_head.value.compare_exchange_strong(consumer_ticket, consumer_ticket + 1,...)) continue;`
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
The original algorithm (liblfds) is based on Dmitry Vyukov's lock-free queue and is not lock-free
as is discussed in this [thread](https://stackoverflow.com/a/54755605).

The original library blocks the _head and _tail pointers and the associated threads
until the ticket requirement defined by **FULL** and **EMPTY** rules is satisfied.
In other words, while a threads is blocked waiting for its reserved slot,
the other threads are also blocked as the _head and _tail pointers
are only advanced when the thread achieves to satisfy the ticket condition.
In summary push function of the original algorithm is as follows:

```
producer_ticket = _tail.load()
INFINITE LOOP
  IF producer_ticket == slot._expected_ticket.load()
    IF _tail.CAS(producer_ticket, producer_ticket + 1)
      BREAK
Push the data
slot._expected_ticket.store(producer_ticket + 1);
```

My push function:

```
producer_ticket = _tail.fetch_add();`
while(!slot._expected_ticket.load() != producer_ticket);`
Push the data`
slot._expected_ticket.store(producer_ticket + 1);`
```

The difference between the two algorithms is:
- liblfds advances the _tail pointer when the two conditions are satisfied:\
`IF producer_ticket == slot._expected_ticket.load()`\
`IF _tail.CAS succeeds`
- My push function advances the _tail pointer non-conditionally.

liblfds blocks all threads when one stalls
due to this conditional pointer advance.
The reason behind this conditional pointer advance is
to keep the FIFO order TEMPORALLY SAFE.
The thread coming first shall write/read first.
However, here in this design, the two pointers always advance.
Hence, the FIFO order is preserved ONLY LOGICALLY BUT NOT TEMPORARILY.
A producer thread (PT1) arriving earlier may be blocked and write later
than another producer (PT2) arriving later.
Correspondingly, the data of PT2 will be read before that of PT1.
This is SAME AS [moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue.git).

liblfds follows the basic invariant of the queue data structure loosing the lock-freedom.
However, advancing the _head or _tail pointers unconditionally
provides lock-freedom but does not preserve the FIFO order.

### 2.1.6. Notes <a id='sec216'></a>
1. Memory orders are chosen to
release data before the visibility of the state transitions and
to acquire data after observing the state transitions.
2. push back-pressures when the queue is full by spinning on its reserved slot while
pop back-pressures when the queue is empty by spinning on its reserved slot.
3. The optimizations for single producer/consumer configurations
can be found in the following header files:\
- queue_LF_ring_MPSC.hpp
- queue_LF_ring_SPMC.hpp
- queue_LF_ring_SPSC.hpp

### 2.1.7. Cautions <a id='sec217'></a>
1. Threads may spin indefinitely if a counterpart thread fails mid-operation,
before setting the expected state accordingly.
2. Use queue_LF_ring_MPMC alias at the end of this file
to get the right specialization of Concurrent_Queue
and to achieve the default arguments consistently.
3. As stated in the Progress section, 
this version does not preserve the FIFO order.

### 2.1.8. TODOs <a id='sec218'></a>
1. The blocking operations (push and pop) back-pressures
waiting the expected ticket of the reserved slot to satisfy ticket invariants.
An exponential backoff strategy is required for these blocking operations.
2. Similar to the 1st one, the edge cases (empty queue and full queue)
requires an exponential backoff strategy as well.
