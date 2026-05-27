#pragma once

#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

namespace voxel::core {

// Unbounded lock-free Multiple-Producer Single-Consumer queue.
//
// Algorithm: Dmitry Vyukov's intrusive MPSC linked list.
//   - Producers push by atomically exchanging the head pointer with a
//     new node, then linking the old head's `next` to the new node.
//     2 atomic ops per push, wait-free per producer.
//   - The consumer pops by reading tail->next, advancing tail, freeing
//     the old node. 1 atomic load per pop, wait-free for the consumer.
//
// Properties:
//   - Lock-free: no mutex, no spin, no priority inversion.
//   - Wait-free per producer (single exchange + store, no retry loop).
//   - Wait-free for the consumer (single load + delete).
//   - Strong FIFO: pop order matches the global push order across all
//     producers (linearizable on push).
//   - No ABA hazard: nodes are deleted, never reused. Allocation cost
//     per item is the dominant overhead (~30-50 ns with default new).
//
// Use cases that fit this shape:
//   - N worker threads producing results, 1 main thread consuming.
//     (Matches the ChunkJobMailbox push/drain pattern.)
//   - Background I/O completions feeding a UI thread.
//   - Any "fan-in" pipeline stage.
//
// Use cases that DON'T fit:
//   - Multiple consumers (use MPMC like moodycamel::ConcurrentQueue).
//   - Hard real-time bounds (alloc latency under memory pressure is
//     not bounded — use a bounded ring buffer if you need that).
//   - Streaming of large objects (the move-construct on pop happens
//     in the consumer; consider pushing pointers/handles instead).
//
// False-sharing protection: head_ and tail_ are alignas(64) so they
// live on separate cache lines. This is essential — producer writes
// to head_ would otherwise invalidate the consumer's tail_ cache line
// (and vice versa) on every operation, costing 50-100 ns per call.
template <typename T>
class MpscQueue {
public:
    MpscQueue()
    {
        auto* sentinel = new Node();
        head_.store(sentinel, std::memory_order_relaxed);
        tail_ = sentinel;
    }

    ~MpscQueue()
    {
        // Drain remaining items. Single-threaded at this point (caller
        // must ensure no producers are still running).
        Node* node = tail_;
        while (node != nullptr) {
            Node* next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }
    }

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;

    // Push a value. Wait-free per producer — no retry loop, two atomic
    // ops (exchange + store). Safe to call from any thread concurrently.
    void push(T value)
    {
        auto* node = new Node();
        node->value = std::move(value);
        // The acquire-release ordering on exchange synchronizes with the
        // consumer's acquire load of tail_->next: when the consumer sees
        // our new node via `next`, it also sees our write to `value`.
        Node* prev = head_.exchange(node, std::memory_order_acq_rel);
        // After exchange, `prev` is exclusively owned by us until we
        // store node into its next pointer. The release ensures the
        // consumer sees `value` initialized before observing `next`.
        prev->next.store(node, std::memory_order_release);
    }

    // Try to pop one value into `out`. Returns true if a value was
    // popped, false if the queue was empty at the moment of the call.
    // MUST only be called by the (single) consumer thread.
    [[nodiscard]] bool try_pop(T& out)
    {
        // Read the sentinel's next pointer. If null, queue is empty.
        // The acquire load synchronizes with a producer's release store
        // — once we see the new node, we also see its value.
        Node* next = tail_->next.load(std::memory_order_acquire);
        if (next == nullptr) {
            return false;
        }
        out = std::move(next->value);
        // Advance tail; the old tail becomes garbage. We can delete it
        // immediately because no producer holds a reference to it
        // (producers only touch head_->next via the exchange chain).
        Node* old_tail = tail_;
        tail_ = next;
        delete old_tail;
        return true;
    }

    // Drain everything currently in the queue into a vector. Convenience
    // method for the common "consumer drains and processes in batch"
    // pattern. Consumer-only.
    //
    // Note: items pushed *during* this drain may or may not be included,
    // depending on timing. For most use cases this is exactly what you
    // want (drain-once semantics) — call drain() again next tick to
    // pick up the latecomers.
    [[nodiscard]] std::vector<T> drain()
    {
        std::vector<T> result;
        T item;
        while (try_pop(item)) {
            result.push_back(std::move(item));
        }
        return result;
    }

    // Drain at most `maxItems` from the queue. Useful when the consumer
    // wants a bounded per-frame budget (drain N now, leave the rest for
    // next frame). Consumer-only.
    [[nodiscard]] std::vector<T> drain(std::size_t maxItems)
    {
        std::vector<T> result;
        result.reserve(maxItems);
        T item;
        while (result.size() < maxItems && try_pop(item)) {
            result.push_back(std::move(item));
        }
        return result;
    }

    // Approximate emptiness check. Lock-free, but the answer may be
    // stale by the time you act on it (a producer could push right
    // after this returns true). Consumer-only.
    [[nodiscard]] bool empty() const noexcept
    {
        return tail_->next.load(std::memory_order_acquire) == nullptr;
    }

private:
    struct Node {
        T value{};
        std::atomic<Node*> next{nullptr};
    };

    // alignas(64) puts head_ and tail_ on separate cache lines. Without
    // this, every producer push would invalidate the consumer's cached
    // tail_ value and vice versa — a ~50 ns false-sharing penalty per
    // operation that dwarfs the actual atomic op cost.
    alignas(64) std::atomic<Node*> head_;  // producers exchange here
    alignas(64) Node* tail_;               // consumer reads tail_->next
};

} // namespace voxel::core
