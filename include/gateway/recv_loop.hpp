#pragma once

#include "gateway/config.hpp"
#include "gateway/source_limiter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace gateway {

// Result status for a single recv operation
enum class RecvStatus : std::uint8_t {
    Ok,                // Successfully received a datagram
    Truncated,         // Datagram exceeded max size (MSG_TRUNC)
    Error,             // System error during recv
    WouldBlock         // No data available (non-blocking mode)
};

// A received datagram with source information
struct Datagram {
    std::vector<std::byte> data;
    SourceKey source;
};

// Result of a recv operation
struct RecvResult {
    RecvStatus status;
    Datagram datagram;         // Valid only if status == Ok
    int error_code = 0;        // errno if status == Error
};

// Metrics for recv operations
struct RecvMetrics {
    std::uint64_t received = 0;       // Successfully received
    std::uint64_t truncated = 0;      // Dropped due to MSG_TRUNC
    std::uint64_t errors = 0;         // System errors
};

// Low-level UDP receiver with TB-1 enforcement.
//
// Responsibilities:
// - Configure socket options (SO_RCVBUF, IP_PMTUDISC_DO)
// - Enforce max datagram size at recv (MSG_TRUNC detection)
// - Extract source IP:port for rate limiting
//
// Thread safety: NOT thread-safe. One RecvLoop per thread.
class RecvLoop {
public:
    // Construct with an already-bound UDP socket fd.
    // Takes ownership: will NOT close fd on destruction (caller manages lifetime).
    explicit RecvLoop(int fd, RecvConfig config = {});

    // Configure socket options. Call once after construction.
    // Returns false if any setsockopt fails.
    bool configure_socket();

    // Receive a single datagram.
    // Blocks until data available (unless socket is non-blocking).
    RecvResult recv_one();

    // Access metrics
    [[nodiscard]] const RecvMetrics& metrics() const noexcept { return metrics_; }

    // Get configured max datagram size
    [[nodiscard]] std::size_t max_datagram_bytes() const noexcept {
        return config_.max_datagram_bytes;
    }

private:
    int fd_;
    RecvConfig config_;
    std::vector<std::byte> buffer_;  // Reusable recv buffer
    RecvMetrics metrics_;
};

// Utility: Create and bind a UDP socket
// Returns fd on success, -1 on failure
int create_udp_socket(std::uint16_t port);

}  // namespace gateway
