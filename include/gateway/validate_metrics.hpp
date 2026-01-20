#pragma once

#include "gateway/parse_metrics.hpp"
#include "gateway/validate_config.hpp"

#include <cstdint>
#include <cmath>
#include <variant>

namespace gateway {

// ============================================================================
// TB-4 Metrics Validation
//
// Semantic validation of parsed metrics messages.
//
// Invariants enforced:
// 1. Semantic validity: timestamps within window, values within range
// 2. Required fields: all required fields present (checked in TB-3)
// 3. agent_id format: ^[a-zA-Z][a-zA-Z0-9_-]{0,63}$
// 4. Bounded CPU: O(metric_count) validation work
// ============================================================================

// Per-metric value validation rules
struct MetricValueRules {
    double min_value = -1e15;
    double max_value = 1e15;
    bool reject_nan = true;
    bool reject_infinity = true;
};

// Full metrics validation configuration
struct MetricsValidationConfig {
    TimestampWindow timestamp_window = kDefaultTimestampWindow;
    MetricValueRules value_rules = {};
    bool require_timestamp = true;            // if true, ts=0 is rejected
};

// TB-4 validation drop reasons (semantic failures)
// These are distinct from TB-3 parsing drop reasons.
enum class MetricsValidationDrop : std::uint8_t {
    // Timestamp issues
    TimestampMissing,         // ts = 0 when require_timestamp = true
    TimestampTooOld,          // ts < current_time - max_age_ms
    TimestampInFuture,        // ts > current_time + max_future_ms

    // agent_id issues
    AgentIdEmpty,             // agent_id is empty string
    AgentIdTooLong,           // agent_id > 64 chars
    AgentIdInvalidFormat,     // doesn't match ^[a-zA-Z][a-zA-Z0-9_-]*$

    // Metric value issues
    MetricValueNaN,           // value is NaN
    MetricValueInfinity,      // value is +/- infinity
    MetricValueTooLow,        // value < min_value
    MetricValueTooHigh,       // value > max_value

    // Metric name issues
    MetricNameEmpty,          // metric name is empty
};

// Validated metrics (semantically valid, ready for normalization)
// Contains views into the original parsed data.
struct ValidatedMetrics {
    std::string_view agent_id;
    std::uint32_t seq;
    std::uint64_t ts;
    const Metric* metrics;    // pointer to first metric
    std::size_t metric_count;
};

// Result type: success or explicit drop reason
using MetricsValidationResult = std::variant<ValidatedMetrics, MetricsValidationDrop>;

// TB-4: Validate parsed metrics against semantic rules.
//
// Precondition: metrics passed TB-3 parsing successfully.
//
// Contract:
// - CPU: O(metric_count) - bounded by MetricsLimits::kMaxMetrics (50)
// - Memory: O(1) - returns views, no allocation
// - Never throws
// - current_time_ms: server's current time for timestamp validation
MetricsValidationResult validate_metrics(
    const ParsedMetrics& parsed,
    const MetricsValidationConfig& config,
    std::uint64_t current_time_ms
) noexcept;

// Default configuration
inline constexpr MetricsValidationConfig kDefaultMetricsValidation = {};

}  // namespace gateway
