#pragma once

#include <cstddef>
#include <cstdint>

namespace gateway {

// Source rate limiter configuration
// Controls per-source fairness and bounded state growth
struct SourceLimiterConfig {
    std::size_t max_sources = 1024;        // LRU cache capacity
    std::uint32_t tokens_per_sec = 100;    // sustained rate (token refill)
    std::uint32_t burst_tokens = 200;      // max tokens (bucket size)
};

// Bounded work queue configuration
// Controls total work bounding and drop behavior
struct QueueConfig {
    std::size_t capacity = 1024;           // max queued datagrams
};

// Receive loop configuration
// Controls TB-1 enforcement: size limits, socket buffers
struct RecvConfig {
    std::size_t max_datagram_bytes = 1472; // MTU(1500) - IP(20) - UDP(8)
    std::size_t recv_buffer_bytes = 256 * 1024;  // SO_RCVBUF hint
};

// Top-level gateway configuration
struct GatewayConfig {
    SourceLimiterConfig source_limiter;
    QueueConfig queue;
    RecvConfig recv;
};

// Conservative defaults suitable for learning/testing
inline constexpr GatewayConfig kDefaultConfig = {};

}  // namespace gateway
