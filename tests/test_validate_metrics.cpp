#include "gateway/validate_metrics.hpp"
#include "gateway/parse_metrics.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

// TB-4 Metrics validation tests.
// Tests semantic validation of parsed metrics.

namespace {

constexpr std::uint64_t kCurrentTime = 1705689600000;  // Fixed "now" for tests

bool is_validation_drop(const gateway::MetricsValidationResult& r,
                        gateway::MetricsValidationDrop reason) {
    if (const auto* dr = std::get_if<gateway::MetricsValidationDrop>(&r)) {
        return *dr == reason;
    }
    return false;
}

const gateway::ValidatedMetrics* get_validated(const gateway::MetricsValidationResult& r) {
    return std::get_if<gateway::ValidatedMetrics>(&r);
}

// Helper to parse and validate in one step
gateway::MetricsValidationResult parse_and_validate(
    std::string_view json,
    const gateway::MetricsValidationConfig& config = gateway::kDefaultMetricsValidation,
    std::uint64_t current_time = kCurrentTime
) {
    auto parse_result = gateway::parse_metrics(json);
    if (const auto* parsed = std::get_if<gateway::ParsedMetrics>(&parse_result)) {
        return gateway::validate_metrics(*parsed, config, current_time);
    }
    // Parse failed - return a sentinel (this shouldn't happen in our tests)
    std::printf("Parse failed unexpectedly\n");
    std::exit(EXIT_FAILURE);
}

}  // namespace

int main() {
    // =========================================================================
    // Success path tests
    // =========================================================================

    // Test 1: Valid metrics message
    {
        std::string json = R"({
            "agent_id": "NodeAlpha",
            "seq": 100,
            "ts": )" + std::to_string(kCurrentTime) + R"(,
            "metrics": [{"n": "cpu", "v": 75.5}]
        })";

        auto r = parse_and_validate(json);
        const auto* v = get_validated(r);
        if (v == nullptr) {
            std::printf("Test 1 failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (v->agent_id != "NodeAlpha") {
            std::printf("Test 1 failed: wrong agent_id\n");
            return EXIT_FAILURE;
        }
    }

    // Test 2: Valid with timestamp at edge of window (exactly 5 min ago)
    {
        std::uint64_t old_ts = kCurrentTime - 300'000;  // exactly 5 min ago
        std::string json = R"({
            "agent_id": "Node1",
            "seq": 1,
            "ts": )" + std::to_string(old_ts) + R"(,
            "metrics": [{"n": "m", "v": 1}]
        })";

        auto r = parse_and_validate(json);
        if (get_validated(r) == nullptr) {
            std::printf("Test 2 failed: timestamp at boundary should be valid\n");
            return EXIT_FAILURE;
        }
    }

    // Test 3: Valid with timestamp at edge of future window (exactly 1 min ahead)
    {
        std::uint64_t future_ts = kCurrentTime + 60'000;  // exactly 1 min ahead
        std::string json = R"({
            "agent_id": "Node1",
            "seq": 1,
            "ts": )" + std::to_string(future_ts) + R"(,
            "metrics": [{"n": "m", "v": 1}]
        })";

        auto r = parse_and_validate(json);
        if (get_validated(r) == nullptr) {
            std::printf("Test 3 failed: timestamp at future boundary should be valid\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // agent_id validation tests
    // =========================================================================

    // Test 4: agent_id starting with number -> invalid
    {
        std::string json = R"({"agent_id": "1node", "seq": 1, "ts": )" +
                          std::to_string(kCurrentTime) + R"(, "metrics": []})";
        auto r = parse_and_validate(json);
        if (!is_validation_drop(r, gateway::MetricsValidationDrop::AgentIdInvalidFormat)) {
            std::printf("Test 4 failed: agent_id starting with number should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 5: agent_id with invalid characters -> invalid
    {
        std::string json = R"({"agent_id": "node@host", "seq": 1, "ts": )" +
                          std::to_string(kCurrentTime) + R"(, "metrics": []})";
        // This should fail at parse time (TB-3), but let's check validation too
        auto parse_result = gateway::parse_metrics(json);
        if (std::get_if<gateway::ParsedMetrics>(&parse_result) != nullptr) {
            // If parsing succeeded, validation should catch it
            auto r = parse_and_validate(json);
            if (!is_validation_drop(r, gateway::MetricsValidationDrop::AgentIdInvalidFormat)) {
                std::printf("Test 5 failed: agent_id with @ should be rejected\n");
                return EXIT_FAILURE;
            }
        }
        // If parse failed, that's also acceptable
    }

    // Test 6: Valid agent_id formats
    {
        const char* valid_ids[] = {
            "a", "A", "node", "Node", "NODE",
            "node-1", "node_1", "Node-Alpha-1",
            "a1", "A1", "aB", "Ab",
            "node-with-many-hyphens",
            "node_with_underscores_123"
        };

        for (const char* id : valid_ids) {
            std::string json = R"({"agent_id": ")" + std::string(id) +
                              R"(", "seq": 1, "ts": )" + std::to_string(kCurrentTime) +
                              R"(, "metrics": []})";
            auto r = parse_and_validate(json);
            if (get_validated(r) == nullptr) {
                std::printf("Test 6 failed: agent_id '%s' should be valid\n", id);
                return EXIT_FAILURE;
            }
        }
    }

    // =========================================================================
    // Timestamp validation tests
    // =========================================================================

    // Test 7: Timestamp too old -> reject
    {
        std::uint64_t old_ts = kCurrentTime - 300'001;  // 1ms too old
        std::string json = R"({
            "agent_id": "Node1",
            "seq": 1,
            "ts": )" + std::to_string(old_ts) + R"(,
            "metrics": [{"n": "m", "v": 1}]
        })";

        auto r = parse_and_validate(json);
        if (!is_validation_drop(r, gateway::MetricsValidationDrop::TimestampTooOld)) {
            std::printf("Test 7 failed: timestamp too old should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 8: Timestamp in future -> reject
    {
        std::uint64_t future_ts = kCurrentTime + 60'001;  // 1ms too far in future
        std::string json = R"({
            "agent_id": "Node1",
            "seq": 1,
            "ts": )" + std::to_string(future_ts) + R"(,
            "metrics": [{"n": "m", "v": 1}]
        })";

        auto r = parse_and_validate(json);
        if (!is_validation_drop(r, gateway::MetricsValidationDrop::TimestampInFuture)) {
            std::printf("Test 8 failed: timestamp in future should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 9: Missing timestamp when required -> reject
    {
        gateway::MetricsValidationConfig config = gateway::kDefaultMetricsValidation;
        config.require_timestamp = true;

        std::string json = R"({"agent_id": "Node1", "seq": 1, "metrics": []})";
        auto r = parse_and_validate(json, config);
        if (!is_validation_drop(r, gateway::MetricsValidationDrop::TimestampMissing)) {
            std::printf("Test 9 failed: missing timestamp should be rejected when required\n");
            return EXIT_FAILURE;
        }
    }

    // Test 10: Missing timestamp when not required -> accept
    {
        gateway::MetricsValidationConfig config = gateway::kDefaultMetricsValidation;
        config.require_timestamp = false;

        std::string json = R"({"agent_id": "Node1", "seq": 1, "metrics": []})";
        auto r = parse_and_validate(json, config);
        if (get_validated(r) == nullptr) {
            std::printf("Test 10 failed: missing timestamp should be accepted when not required\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // Metric value validation tests
    // =========================================================================

    // Test 11: NaN value -> reject
    {
        // We can't easily produce NaN through JSON parsing, so we test directly
        gateway::ParsedMetrics parsed{};
        parsed.agent_id = "Node1";
        parsed.seq = 1;
        parsed.ts = kCurrentTime;
        parsed.metric_count = 1;

        gateway::Metric m{};
        m.name = "cpu";
        m.value = std::numeric_limits<double>::quiet_NaN();
        m.tag_count = 0;
        parsed.metrics[0] = m;

        auto r = gateway::validate_metrics(parsed, gateway::kDefaultMetricsValidation, kCurrentTime);
        if (!is_validation_drop(r, gateway::MetricsValidationDrop::MetricValueNaN)) {
            std::printf("Test 11 failed: NaN value should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 12: Infinity value -> reject
    {
        gateway::ParsedMetrics parsed{};
        parsed.agent_id = "Node1";
        parsed.seq = 1;
        parsed.ts = kCurrentTime;
        parsed.metric_count = 1;

        gateway::Metric m{};
        m.name = "cpu";
        m.value = std::numeric_limits<double>::infinity();
        m.tag_count = 0;
        parsed.metrics[0] = m;

        auto r = gateway::validate_metrics(parsed, gateway::kDefaultMetricsValidation, kCurrentTime);
        if (!is_validation_drop(r, gateway::MetricsValidationDrop::MetricValueInfinity)) {
            std::printf("Test 12 failed: infinity value should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 13: Value below min -> reject
    {
        gateway::MetricsValidationConfig config = gateway::kDefaultMetricsValidation;
        config.value_rules.min_value = 0.0;

        std::string json = R"({
            "agent_id": "Node1",
            "seq": 1,
            "ts": )" + std::to_string(kCurrentTime) + R"(,
            "metrics": [{"n": "m", "v": -1}]
        })";

        auto r = parse_and_validate(json, config);
        if (!is_validation_drop(r, gateway::MetricsValidationDrop::MetricValueTooLow)) {
            std::printf("Test 13 failed: value below min should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 14: Value above max -> reject
    {
        gateway::MetricsValidationConfig config = gateway::kDefaultMetricsValidation;
        config.value_rules.max_value = 100.0;

        std::string json = R"({
            "agent_id": "Node1",
            "seq": 1,
            "ts": )" + std::to_string(kCurrentTime) + R"(,
            "metrics": [{"n": "m", "v": 101}]
        })";

        auto r = parse_and_validate(json, config);
        if (!is_validation_drop(r, gateway::MetricsValidationDrop::MetricValueTooHigh)) {
            std::printf("Test 14 failed: value above max should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 15: NaN allowed when configured
    {
        gateway::MetricsValidationConfig config = gateway::kDefaultMetricsValidation;
        config.value_rules.reject_nan = false;

        gateway::ParsedMetrics parsed{};
        parsed.agent_id = "Node1";
        parsed.seq = 1;
        parsed.ts = kCurrentTime;
        parsed.metric_count = 1;

        gateway::Metric m{};
        m.name = "cpu";
        m.value = std::numeric_limits<double>::quiet_NaN();
        m.tag_count = 0;
        parsed.metrics[0] = m;

        auto r = gateway::validate_metrics(parsed, config, kCurrentTime);
        if (get_validated(r) == nullptr) {
            std::printf("Test 15 failed: NaN should be accepted when configured\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // Edge case tests
    // =========================================================================

    // Test 16: Empty metrics array is valid
    {
        std::string json = R"({
            "agent_id": "Node1",
            "seq": 1,
            "ts": )" + std::to_string(kCurrentTime) + R"(,
            "metrics": []
        })";

        auto r = parse_and_validate(json);
        if (get_validated(r) == nullptr) {
            std::printf("Test 16 failed: empty metrics should be valid\n");
            return EXIT_FAILURE;
        }
    }

    // Test 17: Maximum length agent_id (64 chars) is valid
    {
        std::string long_id = "A" + std::string(63, 'a');  // 64 chars, starts with letter
        std::string json = R"({"agent_id": ")" + long_id +
                          R"(", "seq": 1, "ts": )" + std::to_string(kCurrentTime) +
                          R"(, "metrics": []})";
        auto r = parse_and_validate(json);
        if (get_validated(r) == nullptr) {
            std::printf("Test 17 failed: 64-char agent_id should be valid\n");
            return EXIT_FAILURE;
        }
    }

    // Test 18: Metric at value boundaries (exactly min/max)
    {
        gateway::MetricsValidationConfig config = gateway::kDefaultMetricsValidation;
        config.value_rules.min_value = -100.0;
        config.value_rules.max_value = 100.0;

        // Test min boundary
        std::string json_min = R"({
            "agent_id": "Node1",
            "seq": 1,
            "ts": )" + std::to_string(kCurrentTime) + R"(,
            "metrics": [{"n": "m", "v": -100}]
        })";

        auto r_min = parse_and_validate(json_min, config);
        if (get_validated(r_min) == nullptr) {
            std::printf("Test 18 failed: value at min boundary should be valid\n");
            return EXIT_FAILURE;
        }

        // Test max boundary
        std::string json_max = R"({
            "agent_id": "Node1",
            "seq": 1,
            "ts": )" + std::to_string(kCurrentTime) + R"(,
            "metrics": [{"n": "m", "v": 100}]
        })";

        auto r_max = parse_and_validate(json_max, config);
        if (get_validated(r_max) == nullptr) {
            std::printf("Test 18 failed: value at max boundary should be valid\n");
            return EXIT_FAILURE;
        }
    }

    std::printf("All validate_metrics tests passed\n");
    return EXIT_SUCCESS;
}
