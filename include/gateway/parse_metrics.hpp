#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <array>

namespace gateway {

// ============================================================================
// TB-3 Metrics parsing: JSON schema validation with bounded memory and CPU.
//
// Invariants enforced:
// 1. Memory: All allocations bounded by compile-time constants.
// 2. CPU: Parsing work bounded by input size and fixed iteration limits.
// ============================================================================

// Schema limits (compile-time constants for bounded allocation)
struct MetricsLimits {
    static constexpr std::size_t kMaxAgentIdLen = 64;
    static constexpr std::size_t kMaxMetrics = 50;
    static constexpr std::size_t kMaxMetricNameLen = 128;
    static constexpr std::size_t kMaxUnitLen = 16;
    static constexpr std::size_t kMaxTags = 8;
    static constexpr std::size_t kMaxTagKeyLen = 64;
    static constexpr std::size_t kMaxTagValueLen = 64;
    static constexpr std::size_t kMaxInputBytes = 65536;  // 64KB max input
    static constexpr std::size_t kMaxNestingDepth = 4;
};

// Drop reasons for metrics parsing (explicit enum, not attacker-controlled)
enum class MetricsDropReason : std::uint8_t {
    InputTooLarge,        // Input exceeds kMaxInputBytes
    InvalidJson,          // Malformed JSON syntax
    NestingTooDeep,       // Exceeds kMaxNestingDepth
    MissingRequiredField, // agent_id, seq, or metrics missing
    AgentIdTooLong,       // agent_id exceeds kMaxAgentIdLen
    AgentIdInvalidChars,  // agent_id contains invalid characters
    TooManyMetrics,       // metrics array exceeds kMaxMetrics
    MetricNameTooLong,    // metric name exceeds kMaxMetricNameLen
    MetricMissingName,    // metric missing "n" field
    MetricMissingValue,   // metric missing "v" field
    MetricValueNotNumber, // metric "v" is not a number
    UnitTooLong,          // unit exceeds kMaxUnitLen
    TooManyTags,          // tags exceed kMaxTags
    TagKeyTooLong,        // tag key exceeds kMaxTagKeyLen
    TagValueTooLong,      // tag value exceeds kMaxTagValueLen
    UnexpectedField,      // field not in schema (additionalProperties: false)
    InvalidFieldType,     // field has wrong type
};

// Single tag (key-value pair, views into original input)
struct MetricTag {
    std::string_view key;
    std::string_view value;
};

// Single metric entry (views into original input)
struct Metric {
    std::string_view name;           // "n" field
    double value;                    // "v" field
    std::string_view unit;           // "u" field (optional, empty if absent)
    std::array<MetricTag, MetricsLimits::kMaxTags> tags;
    std::size_t tag_count;           // actual number of tags
};

// Parsed metrics message (views into original input, no allocation)
struct ParsedMetrics {
    std::string_view agent_id;
    std::uint32_t seq;
    std::uint64_t ts;                // timestamp (optional, 0 if absent)
    std::array<Metric, MetricsLimits::kMaxMetrics> metrics;
    std::size_t metric_count;        // actual number of metrics
};

// Result type: success or explicit drop reason
using MetricsResult = std::variant<ParsedMetrics, MetricsDropReason>;

// TB-3: Parse and validate JSON metrics message.
//
// Precondition: input is the body from TB-2 envelope parsing.
//
// Contract:
// - Validates JSON syntax and schema in single pass
// - Memory: O(1) allocation (fixed-size ParsedMetrics struct)
// - CPU: O(n) where n = input.size(), bounded by kMaxInputBytes
// - Never allocates based on attacker-controlled lengths
// - Never throws
// - Returns views into original input (caller must keep input alive)
MetricsResult parse_metrics(std::span<const std::byte> input) noexcept;

// Convenience overload for string_view
MetricsResult parse_metrics(std::string_view input) noexcept;

} // namespace gateway
