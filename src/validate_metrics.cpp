#include "gateway/validate_metrics.hpp"

#include <cmath>

namespace gateway {

MetricsValidationResult validate_metrics(
    const ParsedMetrics& parsed,
    const MetricsValidationConfig& config,
    std::uint64_t current_time_ms
) noexcept {
    // =========================================================================
    // Validate agent_id
    // =========================================================================

    // Check empty
    if (parsed.agent_id.empty()) {
        return MetricsValidationDrop::AgentIdEmpty;
    }

    // Check length (should be caught by TB-3, but defense in depth)
    if (parsed.agent_id.size() > AgentIdRules::kMaxLength) {
        return MetricsValidationDrop::AgentIdTooLong;
    }

    // Check format: ^[a-zA-Z][a-zA-Z0-9_-]{0,63}$
    if (!validate_agent_id_format(parsed.agent_id.data(), parsed.agent_id.size())) {
        return MetricsValidationDrop::AgentIdInvalidFormat;
    }

    // =========================================================================
    // Validate timestamp
    // =========================================================================

    // Check if timestamp is required and missing
    if (config.require_timestamp && parsed.ts == 0) {
        return MetricsValidationDrop::TimestampMissing;
    }

    // Check timestamp window (only if timestamp is provided)
    if (parsed.ts != 0) {
        if (!validate_timestamp_window(parsed.ts, current_time_ms, config.timestamp_window)) {
            // Determine if too old or in future
            std::uint64_t min_allowed = 0;
            if (current_time_ms > static_cast<std::uint64_t>(config.timestamp_window.max_age_ms)) {
                min_allowed = current_time_ms - static_cast<std::uint64_t>(config.timestamp_window.max_age_ms);
            }

            if (parsed.ts < min_allowed) {
                return MetricsValidationDrop::TimestampTooOld;
            } else {
                return MetricsValidationDrop::TimestampInFuture;
            }
        }
    }

    // =========================================================================
    // Validate each metric
    // CPU: O(metric_count) - bounded by MetricsLimits::kMaxMetrics
    // =========================================================================

    for (std::size_t i = 0; i < parsed.metric_count; ++i) {
        const Metric& m = parsed.metrics[i];

        // Check metric name is not empty
        if (m.name.empty()) {
            return MetricsValidationDrop::MetricNameEmpty;
        }

        // Check for NaN
        if (config.value_rules.reject_nan && std::isnan(m.value)) {
            return MetricsValidationDrop::MetricValueNaN;
        }

        // Check for infinity
        if (config.value_rules.reject_infinity && std::isinf(m.value)) {
            return MetricsValidationDrop::MetricValueInfinity;
        }

        // Check value range (only if not NaN/Inf)
        if (!std::isnan(m.value) && !std::isinf(m.value)) {
            if (m.value < config.value_rules.min_value) {
                return MetricsValidationDrop::MetricValueTooLow;
            }
            if (m.value > config.value_rules.max_value) {
                return MetricsValidationDrop::MetricValueTooHigh;
            }
        }
    }

    // =========================================================================
    // All validations passed - return validated metrics
    // =========================================================================

    ValidatedMetrics result;
    result.agent_id = parsed.agent_id;
    result.seq = parsed.seq;
    result.ts = parsed.ts;
    result.metrics = parsed.metrics.data();
    result.metric_count = parsed.metric_count;

    return result;
}

}  // namespace gateway
