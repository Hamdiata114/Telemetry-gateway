#include "gateway/source_limiter.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

// Fake clock for testing
class FakeClock {
public:
    std::chrono::steady_clock::time_point now() const { return current_; }

    void advance(std::chrono::steady_clock::duration d) { current_ += d; }

    gateway::Clock as_clock() {
        return [this]() { return this->now(); };
    }

private:
    std::chrono::steady_clock::time_point current_ =
        std::chrono::steady_clock::time_point{};
};

bool test_single_source_rate_limited() {
    FakeClock clock;
    gateway::SourceLimiterConfig config{
        .max_sources = 10,
        .tokens_per_sec = 100,
        .burst_tokens = 100  // no burst allowance beyond rate
    };
    gateway::SourceLimiter limiter(config, clock.as_clock());
    gateway::SourceKey src{.ip = 0x0A000001, .port = 12345};

    // First 100 should be allowed (initial burst)
    for (int i = 0; i < 100; ++i) {
        if (limiter.admit(src) != gateway::Admit::Allow) {
            std::printf("Expected Allow at i=%d\n", i);
            return false;
        }
    }

    // 101st should be dropped
    if (limiter.admit(src) != gateway::Admit::Drop) {
        std::printf("Expected Drop after exhausting tokens\n");
        return false;
    }

    return true;
}

bool test_budget_replenishes() {
    FakeClock clock;
    gateway::SourceLimiterConfig config{
        .max_sources = 10,
        .tokens_per_sec = 100,
        .burst_tokens = 100
    };
    gateway::SourceLimiter limiter(config, clock.as_clock());
    gateway::SourceKey src{.ip = 0x0A000001, .port = 1};

    // Exhaust tokens
    for (int i = 0; i < 100; ++i) {
        limiter.admit(src);
    }
    if (limiter.admit(src) != gateway::Admit::Drop) {
        return false;
    }

    // Advance 1 second -> 100 new tokens
    clock.advance(std::chrono::seconds(1));

    if (limiter.admit(src) != gateway::Admit::Allow) {
        std::printf("Expected Allow after refill\n");
        return false;
    }

    return true;
}

bool test_fair_share_across_sources() {
    FakeClock clock;
    gateway::SourceLimiterConfig config{
        .max_sources = 10,
        .tokens_per_sec = 100,
        .burst_tokens = 100
    };
    gateway::SourceLimiter limiter(config, clock.as_clock());

    gateway::SourceKey src_a{.ip = 0x0A000001, .port = 1};
    gateway::SourceKey src_b{.ip = 0x0A000002, .port = 1};

    // Both sources get independent budgets
    for (int i = 0; i < 100; ++i) {
        if (limiter.admit(src_a) != gateway::Admit::Allow) return false;
        if (limiter.admit(src_b) != gateway::Admit::Allow) return false;
    }

    // Both exhausted independently
    if (limiter.admit(src_a) != gateway::Admit::Drop) return false;
    if (limiter.admit(src_b) != gateway::Admit::Drop) return false;

    return true;
}

bool test_lru_eviction() {
    FakeClock clock;
    gateway::SourceLimiterConfig config{
        .max_sources = 2,  // tiny cache for testing
        .tokens_per_sec = 100,
        .burst_tokens = 100
    };
    gateway::SourceLimiter limiter(config, clock.as_clock());

    gateway::SourceKey a{.ip = 1, .port = 1};
    gateway::SourceKey b{.ip = 2, .port = 1};
    gateway::SourceKey c{.ip = 3, .port = 1};

    limiter.admit(a);  // [a]
    limiter.admit(b);  // [b, a]

    if (limiter.tracked_count() != 2) return false;

    limiter.admit(c);  // [c, b] - 'a' evicted

    if (limiter.tracked_count() != 2) return false;
    if (limiter.is_tracked(a)) {
        std::printf("Expected 'a' to be evicted\n");
        return false;
    }
    if (!limiter.is_tracked(b)) return false;
    if (!limiter.is_tracked(c)) return false;

    if (limiter.eviction_count() != 1) return false;

    return true;
}

bool test_lru_access_updates_position() {
    FakeClock clock;
    gateway::SourceLimiterConfig config{
        .max_sources = 2,
        .tokens_per_sec = 100,
        .burst_tokens = 100
    };
    gateway::SourceLimiter limiter(config, clock.as_clock());

    gateway::SourceKey a{.ip = 1, .port = 1};
    gateway::SourceKey b{.ip = 2, .port = 1};
    gateway::SourceKey c{.ip = 3, .port = 1};

    limiter.admit(a);  // [a]
    limiter.admit(b);  // [b, a]
    limiter.admit(a);  // [a, b] - 'a' accessed, moved to front

    limiter.admit(c);  // [c, a] - 'b' evicted (was LRU)

    if (limiter.is_tracked(b)) {
        std::printf("Expected 'b' to be evicted, not 'a'\n");
        return false;
    }
    if (!limiter.is_tracked(a)) return false;
    if (!limiter.is_tracked(c)) return false;

    return true;
}

bool test_bounded_state_growth() {
    FakeClock clock;
    gateway::SourceLimiterConfig config{
        .max_sources = 100,  // hard cap
        .tokens_per_sec = 100,
        .burst_tokens = 100
    };
    gateway::SourceLimiter limiter(config, clock.as_clock());

    // Add 1000 unique sources
    for (std::uint32_t i = 0; i < 1000; ++i) {
        gateway::SourceKey src{.ip = i, .port = 1};
        limiter.admit(src);
    }

    // Should never exceed max_sources
    if (limiter.tracked_count() > 100) {
        std::printf("State grew beyond max_sources: %zu\n", limiter.tracked_count());
        return false;
    }

    // Should have evicted 900 entries
    if (limiter.eviction_count() != 900) {
        std::printf("Expected 900 evictions, got %llu\n", limiter.eviction_count());
        return false;
    }

    return true;
}

bool test_metrics() {
    FakeClock clock;
    gateway::SourceLimiterConfig config{
        .max_sources = 10,
        .tokens_per_sec = 100,
        .burst_tokens = 5  // small burst for easy testing
    };
    gateway::SourceLimiter limiter(config, clock.as_clock());
    gateway::SourceKey src{.ip = 1, .port = 1};

    // 5 admits, 3 drops
    for (int i = 0; i < 8; ++i) {
        limiter.admit(src);
    }

    if (limiter.total_admits() != 5) {
        std::printf("Expected 5 admits, got %llu\n", limiter.total_admits());
        return false;
    }
    if (limiter.total_drops() != 3) {
        std::printf("Expected 3 drops, got %llu\n", limiter.total_drops());
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!test_single_source_rate_limited()) {
        std::printf("test_single_source_rate_limited failed\n");
        return EXIT_FAILURE;
    }

    if (!test_budget_replenishes()) {
        std::printf("test_budget_replenishes failed\n");
        return EXIT_FAILURE;
    }

    if (!test_fair_share_across_sources()) {
        std::printf("test_fair_share_across_sources failed\n");
        return EXIT_FAILURE;
    }

    if (!test_lru_eviction()) {
        std::printf("test_lru_eviction failed\n");
        return EXIT_FAILURE;
    }

    if (!test_lru_access_updates_position()) {
        std::printf("test_lru_access_updates_position failed\n");
        return EXIT_FAILURE;
    }

    if (!test_bounded_state_growth()) {
        std::printf("test_bounded_state_growth failed\n");
        return EXIT_FAILURE;
    }

    if (!test_metrics()) {
        std::printf("test_metrics failed\n");
        return EXIT_FAILURE;
    }

    std::printf("All source_limiter tests passed\n");
    return EXIT_SUCCESS;
}
