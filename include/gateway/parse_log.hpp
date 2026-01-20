#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <array>

namespace gateway {

// ============================================================================
// TB-3 Log parsing: logfmt format with bounded memory and CPU.
//
// Format: key=value pairs separated by spaces, one log per message.
// Example: ts=1705689600000 level=error agent=node-42 msg="Connection refused"
//
// Invariants enforced:
// 1. Memory: All allocations bounded by compile-time constants.
// 2. CPU: Single-pass O(n) parsing, no backtracking.
// ============================================================================

// Schema limits (compile-time constants for bounded allocation)
struct LogLimits {
    static constexpr std::size_t kMaxLineBytes = 2048;
    static constexpr std::size_t kMaxFields = 16;
    static constexpr std::size_t kMaxKeyLen = 32;
    static constexpr std::size_t kMaxValueLen = 1024;
};

// Log severity levels (explicit enum)
enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
};

// Drop reasons for log parsing (explicit enum, not attacker-controlled)
enum class LogDropReason : std::uint8_t {
    InputTooLarge,        // Input exceeds kMaxLineBytes
    EmptyInput,           // Input is empty
    TooManyFields,        // Exceeds kMaxFields
    KeyTooLong,           // Key exceeds kMaxKeyLen
    ValueTooLong,         // Value exceeds kMaxValueLen
    InvalidKeyChar,       // Key contains invalid character
    MissingEquals,        // Field missing '=' separator
    UnterminatedQuote,    // Quoted value missing closing quote
    MissingTimestamp,     // Required "ts" field missing
    MissingLevel,         // Required "level" field missing
    MissingMessage,       // Required "msg" field missing
    InvalidTimestamp,     // "ts" is not a valid integer
    InvalidLevel,         // "level" is not a recognized level string
};

// Single log field (key-value pair, views into original input)
struct LogField {
    std::string_view key;
    std::string_view value;
};

// Parsed log entry (views into original input, no allocation)
struct ParsedLog {
    std::uint64_t ts;                // parsed from "ts" field
    LogLevel level;                  // parsed from "level" field
    std::string_view agent_id;       // "agent" field (optional, empty if absent)
    std::string_view msg;            // "msg" field

    // All fields including required ones (for pass-through)
    std::array<LogField, LogLimits::kMaxFields> fields;
    std::size_t field_count;         // actual number of fields
};

// Result type: success or explicit drop reason
using LogResult = std::variant<ParsedLog, LogDropReason>;

// TB-3: Parse and validate logfmt log message.
//
// Precondition: input is the body from TB-2 envelope parsing.
//
// Contract:
// - Parses logfmt syntax in single pass
// - Memory: O(1) allocation (fixed-size ParsedLog struct)
// - CPU: O(n) where n = input.size(), bounded by kMaxLineBytes
// - No regex, no backtracking
// - Never throws
// - Returns views into original input (caller must keep input alive)
LogResult parse_log(std::span<const std::byte> input) noexcept;

// Convenience overload for string_view
LogResult parse_log(std::string_view input) noexcept;

// Convert LogLevel to string (for logging/metrics)
constexpr std::string_view log_level_to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        case LogLevel::Fatal: return "fatal";
    }
    return "unknown";
}

// Parse level string to LogLevel (returns false if invalid)
bool parse_log_level(std::string_view s, LogLevel& out) noexcept;

} // namespace gateway
