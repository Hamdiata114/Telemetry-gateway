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

    std::printf("All bounded_queue tests passed\n");
    return EXIT_SUCCESS;
}
