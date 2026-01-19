#pragma once

#include "gateway/config.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>

namespace gateway {

// Identifies a unique source by IP address and port
struct SourceKey {
    std::uint32_t ip;    // IPv4 address in host byte order
    std::uint16_t port;  // Source port in host byte order

    bool operator==(const SourceKey& other) const noexcept {
        return ip == other.ip && port == other.port;
    }
};

}  // namespace gateway

// Hash specialization for SourceKey
template <>
struct std::hash<gateway::SourceKey> {
    std::size_t operator()(const gateway::SourceKey& k) const noexcept {
        // Combine ip and port into a single hash
        return std::hash<std::uint64_t>{}(
            (static_cast<std::uint64_t>(k.ip) << 16) | k.port
        );
    }
};

namespace gateway {

// Result of admission check
enum class Admit : std::uint8_t {
    Allow,   // Source has budget, packet allowed
    Drop     // Source exhausted budget, packet dropped
};

// Clock abstraction for testing
// Default uses steady_clock, tests can inject fake clock
using Clock = std::function<std::chrono::steady_clock::time_point()>;

inline std::chrono::steady_clock::time_point default_clock() {
    return std::chrono::steady_clock::now();
}

// Per-source rate limiter using token bucket + LRU eviction.
//
// Invariants enforced:
// - Bounds per-source share of processing capacity (token bucket)
// - Bounds total state growth (LRU eviction at max_sources)
//
// Thread safety: NOT thread-safe. External synchronization required.
class SourceLimiter {
public:
    explicit SourceLimiter(SourceLimiterConfig config = {},
                          Clock clock = default_clock);

    // Check if a packet from this source should be admitted.
    // Consumes one token if allowed.
    Admit admit(const SourceKey& source);

    // Current number of tracked sources
    [[nodiscard]] std::size_t tracked_count() const noexcept;

    // Check if a source is currently tracked (for testing)
    [[nodiscard]] bool is_tracked(const SourceKey& source) const;

    // Metrics
    [[nodiscard]] std::uint64_t total_admits() const noexcept { return total_admits_; }
    [[nodiscard]] std::uint64_t total_drops() const noexcept { return total_drops_; }
    [[nodiscard]] std::uint64_t eviction_count() const noexcept { return eviction_count_; }

private:
    // Token bucket state for a single source
    struct Bucket {
        double tokens;  // current token count
        std::chrono::steady_clock::time_point last_update;
    };

    // LRU list stores keys in access order (most recent at front)
    using LruList = std::list<SourceKey>;
    using LruIterator = LruList::iterator;

    // Map from source to (bucket, lru position)
    struct Entry {
        Bucket bucket;
        LruIterator lru_pos;
    };
    using SourceMap = std::unordered_map<SourceKey, Entry>;

    // Refill tokens based on elapsed time
    void refill_bucket(Bucket& bucket);

    // Evict least recently used entry
    void evict_lru();

    SourceLimiterConfig config_;
    Clock clock_;
    SourceMap sources_;
    LruList lru_list_;

    // Metrics
    std::uint64_t total_admits_ = 0;
    std::uint64_t total_drops_ = 0;
    std::uint64_t eviction_count_ = 0;
};

}  // namespace gateway
