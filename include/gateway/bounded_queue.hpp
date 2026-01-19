#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gateway {

// Result of attempting to push to the queue
enum class PushResult : std::uint8_t {
    Ok,
    Dropped  // Queue was full
};

// A bounded, fixed-capacity queue that drops on overflow.
// Provides controlled degradation under overload: drops rather than
// unbounded growth.
//
// Thread safety: NOT thread-safe. External synchronization required
// for concurrent access.
//
// Template parameter T must be movable.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity)
        : buffer_(capacity)
        , capacity_(capacity)
        , head_(0)
        , tail_(0)
        , size_(0)
        , drop_count_(0) {}

    // Attempt to push an item. Returns Dropped if queue is full.
    // Item is moved into the queue on success.
    PushResult try_push(T item) noexcept {
        if (size_ >= capacity_) {
            ++drop_count_;
            return PushResult::Dropped;
        }
        buffer_[tail_] = std::move(item);
        tail_ = (tail_ + 1) % capacity_;
        ++size_;
        return PushResult::Ok;
    }

    // Pop an item from the queue. Returns nullopt if empty.
    std::optional<T> try_pop() noexcept {
        if (size_ == 0) {
            return std::nullopt;
        }
        T item = std::move(buffer_[head_]);
        head_ = (head_ + 1) % capacity_;
        --size_;
        return item;
    }

    // Peek at the front item without removing it.
    // Returns nullptr if empty.
    const T* peek() const noexcept {
        if (size_ == 0) {
            return nullptr;
        }
        return &buffer_[head_];
    }

    // Current number of items in the queue
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // Maximum capacity
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Check if queue is empty
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // Check if queue is full
    [[nodiscard]] bool full() const noexcept { return size_ >= capacity_; }

    // Number of items dropped due to overflow (cumulative)
    [[nodiscard]] std::uint64_t drop_count() const noexcept { return drop_count_; }

    // Reset drop counter (e.g., after reporting metrics)
    void reset_drop_count() noexcept { drop_count_ = 0; }

private:
    std::vector<T> buffer_;
    std::size_t capacity_;
    std::size_t head_;  // index of next item to pop
    std::size_t tail_;  // index of next slot to push
    std::size_t size_;  // current number of items
    std::uint64_t drop_count_;
};

}  // namespace gateway
