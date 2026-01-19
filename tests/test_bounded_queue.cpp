#include "gateway/bounded_queue.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

bool test_basic_push_pop() {
    gateway::BoundedQueue<int> queue(3);

    if (!queue.empty()) return false;
    if (queue.size() != 0) return false;

    // Push three items
    if (queue.try_push(1) != gateway::PushResult::Ok) return false;
    if (queue.try_push(2) != gateway::PushResult::Ok) return false;
    if (queue.try_push(3) != gateway::PushResult::Ok) return false;

    if (queue.size() != 3) return false;
    if (!queue.full()) return false;

    // Pop in FIFO order
    auto v1 = queue.try_pop();
    if (!v1 || *v1 != 1) return false;

    auto v2 = queue.try_pop();
    if (!v2 || *v2 != 2) return false;

    auto v3 = queue.try_pop();
    if (!v3 || *v3 != 3) return false;

    if (!queue.empty()) return false;

    return true;
}

bool test_drop_on_full() {
    gateway::BoundedQueue<int> queue(2);

    queue.try_push(1);
    queue.try_push(2);

    // Queue is full, next push should drop
    if (queue.try_push(3) != gateway::PushResult::Dropped) return false;
    if (queue.drop_count() != 1) return false;

    // Another drop
    if (queue.try_push(4) != gateway::PushResult::Dropped) return false;
    if (queue.drop_count() != 2) return false;

    // Original items still there
    auto v1 = queue.try_pop();
    if (!v1 || *v1 != 1) return false;

    auto v2 = queue.try_pop();
    if (!v2 || *v2 != 2) return false;

    return true;
}

bool test_pop_empty() {
    gateway::BoundedQueue<int> queue(2);

    auto v = queue.try_pop();
    if (v.has_value()) return false;  // Should be nullopt

    return true;
}

bool test_peek() {
    gateway::BoundedQueue<int> queue(2);

    // Peek empty queue
    if (queue.peek() != nullptr) return false;

    queue.try_push(42);

    // Peek should return pointer to front
    const int* p = queue.peek();
    if (p == nullptr || *p != 42) return false;

    // Peek doesn't remove
    if (queue.size() != 1) return false;

    return true;
}

bool test_wrap_around() {
    gateway::BoundedQueue<int> queue(3);

    // Fill and drain twice to test wrap-around
    for (int round = 0; round < 2; ++round) {
        queue.try_push(1);
        queue.try_push(2);
        queue.try_push(3);

        auto v1 = queue.try_pop();
        auto v2 = queue.try_pop();
        auto v3 = queue.try_pop();

        if (!v1 || *v1 != 1) return false;
        if (!v2 || *v2 != 2) return false;
        if (!v3 || *v3 != 3) return false;
    }

    return true;
}

bool test_movable_types() {
    gateway::BoundedQueue<std::string> queue(2);

    queue.try_push("hello");
    queue.try_push("world");

    auto v1 = queue.try_pop();
    if (!v1 || *v1 != "hello") return false;

    auto v2 = queue.try_pop();
    if (!v2 || *v2 != "world") return false;

    return true;
}

bool test_reset_drop_count() {
    gateway::BoundedQueue<int> queue(1);

    queue.try_push(1);
    queue.try_push(2);  // dropped
    queue.try_push(3);  // dropped

    if (queue.drop_count() != 2) return false;

    queue.reset_drop_count();
    if (queue.drop_count() != 0) return false;

    return true;
}

bool test_capacity_one() {
    // Edge case: capacity of exactly 1
    gateway::BoundedQueue<int> queue(1);

    if (!queue.empty()) return false;
    if (queue.full()) return false;
    if (queue.capacity() != 1) return false;

    // Push one item
    if (queue.try_push(42) != gateway::PushResult::Ok) return false;
    if (!queue.full()) return false;
    if (queue.size() != 1) return false;

    // Second push should drop
    if (queue.try_push(99) != gateway::PushResult::Dropped) return false;
    if (queue.drop_count() != 1) return false;

    // Peek should still show 42
    const int* p = queue.peek();
    if (p == nullptr || *p != 42) return false;

    // Pop should return 42
    auto v = queue.try_pop();
    if (!v || *v != 42) return false;

    if (!queue.empty()) return false;

    // Can push again after pop
    if (queue.try_push(100) != gateway::PushResult::Ok) return false;
    if (queue.size() != 1) return false;

    return true;
}

bool test_multiple_consecutive_drops() {
    // Test drop count accumulation with many drops
    gateway::BoundedQueue<int> queue(1);

    queue.try_push(1);  // Queue is now full

    // Drop many items
    for (int i = 0; i < 1000; ++i) {
        if (queue.try_push(i) != gateway::PushResult::Dropped) return false;
    }

    if (queue.drop_count() != 1000) {
        std::printf("Expected drop_count=1000, got %llu\n", queue.drop_count());
        return false;
    }

    // Original item still there
    auto v = queue.try_pop();
    if (!v || *v != 1) return false;

    return true;
}

bool test_peek_after_wraparound() {
    // Ensure peek returns correct pointer after ring buffer wraps
    gateway::BoundedQueue<int> queue(3);

    // Fill and drain to advance head/tail
    for (int round = 0; round < 5; ++round) {
        queue.try_push(10 + round);
        queue.try_push(20 + round);
        queue.try_push(30 + round);

        // Peek should always show the first pushed item
        const int* p = queue.peek();
        if (p == nullptr || *p != 10 + round) {
            std::printf("Peek after wraparound failed at round %d\n", round);
            return false;
        }

        queue.try_pop();
        queue.try_pop();
        queue.try_pop();
    }

    return true;
}

bool test_size_consistency() {
    // Verify size() is always consistent with operations
    gateway::BoundedQueue<int> queue(5);

    for (std::size_t expected = 0; expected <= 5; ++expected) {
        if (queue.size() != expected) {
            std::printf("Size mismatch: expected %zu, got %zu\n", expected, queue.size());
            return false;
        }
        if (expected < 5) {
            queue.try_push(static_cast<int>(expected));
        }
    }

    // Full queue
    if (!queue.full()) return false;
    if (queue.size() != 5) return false;

    // Pop all and verify size decreases
    for (std::size_t i = 5; i > 0; --i) {
        if (queue.size() != i) return false;
        queue.try_pop();
    }

    if (!queue.empty()) return false;
    if (queue.size() != 0) return false;

    return true;
}

bool test_interleaved_push_pop() {
    // Test rapid interleaved push/pop at capacity boundary
    gateway::BoundedQueue<int> queue(2);

    // Fill to capacity
    queue.try_push(1);
    queue.try_push(2);

    // Interleaved operations
    for (int i = 0; i < 100; ++i) {
        // Pop one
        auto v = queue.try_pop();
        if (!v) return false;

        // Push one (should succeed since we just popped)
        if (queue.try_push(i + 3) != gateway::PushResult::Ok) return false;

        // Queue should still be full (size=2)
        if (queue.size() != 2) return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!test_basic_push_pop()) {
        std::printf("test_basic_push_pop failed\n");
        return EXIT_FAILURE;
    }

    if (!test_drop_on_full()) {
        std::printf("test_drop_on_full failed\n");
        return EXIT_FAILURE;
    }

    if (!test_pop_empty()) {
        std::printf("test_pop_empty failed\n");
        return EXIT_FAILURE;
    }

    if (!test_peek()) {
        std::printf("test_peek failed\n");
        return EXIT_FAILURE;
    }

    if (!test_wrap_around()) {
        std::printf("test_wrap_around failed\n");
        return EXIT_FAILURE;
    }

    if (!test_movable_types()) {
        std::printf("test_movable_types failed\n");
        return EXIT_FAILURE;
    }

    if (!test_reset_drop_count()) {
        std::printf("test_reset_drop_count failed\n");
        return EXIT_FAILURE;
    }

    if (!test_capacity_one()) {
        std::printf("test_capacity_one failed\n");
        return EXIT_FAILURE;
    }

    if (!test_multiple_consecutive_drops()) {
        std::printf("test_multiple_consecutive_drops failed\n");
        return EXIT_FAILURE;
    }

    if (!test_peek_after_wraparound()) {
        std::printf("test_peek_after_wraparound failed\n");
        return EXIT_FAILURE;
    }

    if (!test_size_consistency()) {
        std::printf("test_size_consistency failed\n");
        return EXIT_FAILURE;
    }

    if (!test_interleaved_push_pop()) {
        std::printf("test_interleaved_push_pop failed\n");
        return EXIT_FAILURE;
    }

    std::printf("All bounded_queue tests passed\n");
    return EXIT_SUCCESS;
}
