#pragma once

#include "gateway/parse_log.hpp"
#include "gateway/validate_config.hpp"

#include <cstdint>
#include <variant>

namespace gateway {

// ============================================================================
// TB-4 Log Validation
//
// Semantic validation of parsed log messages.
//
// Invariants enforced:
// 1. Semantic validity: timestamps within window
// 2. Required fields: all required fields present (checked in TB-3)
// 3. agent_id format: ^[a-zA-Z][a-zA-Z0-9_-]{0,63}$
// 4. Bounded CPU: O(field_count) validation work
// ============================================================================

// Log validation configuration
struct LogValidationConfig {
    TimestampWindow timestamp_window = kDefaultTimestampWindow;
    LogLevel min_level = LogLevel::Trace;     // reject logs below this level
    std::size_t max_message_length = 1024;    // max message length
    bool truncate_oversized_message = true;   // true = truncate, false = reject
    bool require_agent_id = false;            // if true, empty agent_id is rejected
};

// TB-4 validation drop reasons for logs
enum class LogValidationDrop : std::uint8_t {
    // Timestamp issues
    TimestampTooOld,          // ts < current_time - max_age_ms
    TimestampInFuture,        // ts > current_time + max_future_ms

    // agent_id issues
    AgentIdEmpty,             // agent_id required but empty
    AgentIdTooLong,           // agent_id > 64 chars
    AgentIdInvalidFormat,     // doesn't match ^[a-zA-Z][a-zA-Z0-9_-]*$

    // Log-specific issues
    LevelBelowMinimum,        // log level < min_level
    MessageTooLong,           // when truncate_oversized_message = false
    MessageEmpty,             // message is empty
};

// Validated log (semantically valid, ready for normalization)
// Contains views into the original parsed data.
struct ValidatedLog {
    std::string_view agent_id;    // may be empty if not required
    std::uint64_t ts;
    LogLevel level;
    std::string_view msg;         // possibly truncated
    const LogField* fields;       // pointer to first field
    std::size_t field_count;
};

// Result type: success or explicit drop reason
using LogValidationResult = std::variant<ValidatedLog, LogValidationDrop>;

// TB-4: Validate parsed log against semantic rules.
//
// Precondition: log passed TB-3 parsing successfully.
//
// Contract:
// - CPU: O(field_count) - bounded by LogLimits::kMaxFields (16)
// - Memory: O(1) - returns views, no allocation
// - Never throws
// - current_time_ms: server's current time for timestamp validation
LogValidationResult validate_log(
    const ParsedLog& parsed,
    const LogValidationConfig& config,
    std::uint64_t current_time_ms
) noexcept;

// Default configuration
inline constexpr LogValidationConfig kDefaultLogValidation = {};

}  // namespace gateway
