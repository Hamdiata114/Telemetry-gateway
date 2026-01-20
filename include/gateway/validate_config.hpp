#pragma once

#include <cstdint>
#include <cstddef>

namespace gateway {

// ============================================================================
// TB-4 Validation Configuration
//
// Common configuration types for semantic validation of parsed messages.
// All validation operations are bounded by these configurable limits.
// ============================================================================

// Timestamp validation window (relative to server time)
struct TimestampWindow {
    std::int64_t max_age_ms = 300'000;       // 5 min in the past (default)
    std::int64_t max_future_ms = 60'000;     // 1 min in the future (default)
};

// agent_id validation rules
// Format: ^[a-zA-Z][a-zA-Z0-9_-]{0,63}$
// - Must start with letter [a-zA-Z]
// - Remaining: letters, digits, underscore, hyphen
// - Length: 1-64 characters
struct AgentIdRules {
    static constexpr std::size_t kMinLength = 1;
    static constexpr std::size_t kMaxLength = 64;
};

// Validate agent_id format.
// Returns true if agent_id matches: ^[a-zA-Z][a-zA-Z0-9_-]{0,63}$
// CPU: O(n) where n = agent_id.size(), bounded by kMaxLength
bool validate_agent_id_format(const char* data, std::size_t len) noexcept;

// Validate timestamp is within acceptable window.
// Returns true if: (current_time - max_age) <= ts <= (current_time + max_future)
// CPU: O(1)
bool validate_timestamp_window(
    std::uint64_t ts,
    std::uint64_t current_time_ms,
    const TimestampWindow& window
) noexcept;

// Default validation configuration
inline constexpr TimestampWindow kDefaultTimestampWindow = {
    .max_age_ms = 300'000,      // 5 minutes
    .max_future_ms = 60'000,    // 1 minute
};

}  // namespace gateway
