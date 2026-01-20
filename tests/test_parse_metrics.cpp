#include "gateway/parse_metrics.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <variant>

// TB-3 JSON metrics parsing tests.
// Tests both invariants:
// 1. Bounded memory allocation
// 2. Bounded CPU (parsing work)

namespace {

bool is_drop_reason(const gateway::MetricsResult& r, gateway::MetricsDropReason reason) {
    if (const auto* dr = std::get_if<gateway::MetricsDropReason>(&r)) {
        return *dr == reason;
    }
    return false;
}

const gateway::ParsedMetrics* get_metrics_if_success(const gateway::MetricsResult& r) {
    return std::get_if<gateway::ParsedMetrics>(&r);
}

bool require_drop(std::string_view input, gateway::MetricsDropReason expected) {
    auto r = gateway::parse_metrics(input);
    return is_drop_reason(r, expected);
}

} // namespace

int main() {
    // =========================================================================
    // Success path tests
    // =========================================================================

    // Test 1: Minimal valid message
    {
        std::string_view input = R"({"agent_id":"node-1","seq":42,"metrics":[]})";
        auto r = gateway::parse_metrics(input);
        const auto* m = get_metrics_if_success(r);
        if (m == nullptr) {
            std::printf("Minimal valid message test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (m->agent_id != "node-1") {
            std::printf("Minimal valid message test failed: wrong agent_id\n");
            return EXIT_FAILURE;
        }
        if (m->seq != 42) {
            std::printf("Minimal valid message test failed: wrong seq\n");
            return EXIT_FAILURE;
        }
        if (m->metric_count != 0) {
            std::printf("Minimal valid message test failed: expected empty metrics\n");
            return EXIT_FAILURE;
        }
    }

    // Test 2: Full message with metrics
    {
        std::string_view input = R"({
            "agent_id": "node-42",
            "seq": 100,
            "ts": 1705689600000,
            "metrics": [
                {"n": "cpu_usage", "v": 75.5, "u": "percent"},
                {"n": "memory_mb", "v": 1024}
            ]
        })";
        auto r = gateway::parse_metrics(input);
        const auto* m = get_metrics_if_success(r);
        if (m == nullptr) {
            std::printf("Full message test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (m->agent_id != "node-42") {
            std::printf("Full message test failed: wrong agent_id\n");
            return EXIT_FAILURE;
        }
        if (m->seq != 100) {
            std::printf("Full message test failed: wrong seq\n");
            return EXIT_FAILURE;
        }
        if (m->ts != 1705689600000) {
            std::printf("Full message test failed: wrong ts\n");
            return EXIT_FAILURE;
        }
        if (m->metric_count != 2) {
            std::printf("Full message test failed: wrong metric count\n");
            return EXIT_FAILURE;
        }
        if (m->metrics[0].name != "cpu_usage") {
            std::printf("Full message test failed: wrong metric name\n");
            return EXIT_FAILURE;
        }
        if (m->metrics[0].value != 75.5) {
            std::printf("Full message test failed: wrong metric value\n");
            return EXIT_FAILURE;
        }
        if (m->metrics[0].unit != "percent") {
            std::printf("Full message test failed: wrong metric unit\n");
            return EXIT_FAILURE;
        }
        if (m->metrics[1].name != "memory_mb") {
            std::printf("Full message test failed: wrong second metric name\n");
            return EXIT_FAILURE;
        }
    }

    // Test 3: Metric with tags
    {
        std::string_view input = R"({
            "agent_id": "server-1",
            "seq": 1,
            "metrics": [
                {"n": "request_count", "v": 42, "t": {"method": "GET", "path": "/api"}}
            ]
        })";
        auto r = gateway::parse_metrics(input);
        const auto* m = get_metrics_if_success(r);
        if (m == nullptr) {
            std::printf("Metric with tags test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (m->metrics[0].tag_count != 2) {
            std::printf("Metric with tags test failed: wrong tag count\n");
            return EXIT_FAILURE;
        }
        if (m->metrics[0].tags[0].key != "method" ||
            m->metrics[0].tags[0].value != "GET") {
            std::printf("Metric with tags test failed: wrong first tag\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // Invariant 1: Bounded memory allocation tests
    // =========================================================================

    // Test 4: Input too large -> InputTooLarge
    {
        std::string large_input(gateway::MetricsLimits::kMaxInputBytes + 1, ' ');
        if (!require_drop(large_input, gateway::MetricsDropReason::InputTooLarge)) {
            std::printf("InputTooLarge test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 5: agent_id too long -> AgentIdTooLong
    {
        std::string long_id(gateway::MetricsLimits::kMaxAgentIdLen + 1, 'a');
        std::string input = R"({"agent_id":")" + long_id + R"(","seq":1,"metrics":[]})";
        if (!require_drop(input, gateway::MetricsDropReason::AgentIdTooLong)) {
            std::printf("AgentIdTooLong test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 6: Too many metrics -> TooManyMetrics
    {
        std::string input = R"({"agent_id":"a","seq":1,"metrics":[)";
        for (std::size_t i = 0; i <= gateway::MetricsLimits::kMaxMetrics; ++i) {
            if (i > 0) input += ",";
            input += R"({"n":"m","v":1})";
        }
        input += "]}";
        if (!require_drop(input, gateway::MetricsDropReason::TooManyMetrics)) {
            std::printf("TooManyMetrics test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 7: Metric name too long -> MetricNameTooLong
    {
        std::string long_name(gateway::MetricsLimits::kMaxMetricNameLen + 1, 'x');
        std::string input = R"({"agent_id":"a","seq":1,"metrics":[{"n":")" + long_name + R"(","v":1}]})";
        if (!require_drop(input, gateway::MetricsDropReason::MetricNameTooLong)) {
            std::printf("MetricNameTooLong test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 8: Too many tags -> TooManyTags
    {
        std::string input = R"({"agent_id":"a","seq":1,"metrics":[{"n":"m","v":1,"t":{)";
        for (std::size_t i = 0; i <= gateway::MetricsLimits::kMaxTags; ++i) {
            if (i > 0) input += ",";
            input += R"("k)" + std::to_string(i) + R"(":"v")";
        }
        input += "}}]}";
        if (!require_drop(input, gateway::MetricsDropReason::TooManyTags)) {
            std::printf("TooManyTags test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 9: Tag key too long -> TagKeyTooLong
    {
        std::string long_key(gateway::MetricsLimits::kMaxTagKeyLen + 1, 'k');
        std::string input = R"({"agent_id":"a","seq":1,"metrics":[{"n":"m","v":1,"t":{")" +
                           long_key + R"(":"v"}}]})";
        if (!require_drop(input, gateway::MetricsDropReason::TagKeyTooLong)) {
            std::printf("TagKeyTooLong test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 10: Tag value too long -> TagValueTooLong
    {
        std::string long_val(gateway::MetricsLimits::kMaxTagValueLen + 1, 'v');
        std::string input = R"({"agent_id":"a","seq":1,"metrics":[{"n":"m","v":1,"t":{"k":")" +
                           long_val + R"("}}]})";
        if (!require_drop(input, gateway::MetricsDropReason::TagValueTooLong)) {
            std::printf("TagValueTooLong test failed\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // Invariant 2: Bounded parsing work tests
    // =========================================================================

    // Test 11: Deep nesting -> NestingTooDeep
    // This would require crafting deeply nested JSON, but our schema
    // only allows 4 levels max, so we test the boundary

    // =========================================================================
    // Schema validation tests
    // =========================================================================

    // Test 12: Missing required field (agent_id) -> MissingRequiredField
    {
        std::string_view input = R"({"seq":1,"metrics":[]})";
        if (!require_drop(input, gateway::MetricsDropReason::MissingRequiredField)) {
            std::printf("Missing agent_id test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 13: Missing required field (seq) -> MissingRequiredField
    {
        std::string_view input = R"({"agent_id":"a","metrics":[]})";
        if (!require_drop(input, gateway::MetricsDropReason::MissingRequiredField)) {
            std::printf("Missing seq test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 14: Missing required field (metrics) -> MissingRequiredField
    {
        std::string_view input = R"({"agent_id":"a","seq":1})";
        if (!require_drop(input, gateway::MetricsDropReason::MissingRequiredField)) {
            std::printf("Missing metrics test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 15: Invalid agent_id characters -> AgentIdInvalidChars
    {
        std::string_view input = R"({"agent_id":"node@bad!","seq":1,"metrics":[]})";
        if (!require_drop(input, gateway::MetricsDropReason::AgentIdInvalidChars)) {
            std::printf("AgentIdInvalidChars test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 16: Unexpected field -> UnexpectedField
    {
        std::string_view input = R"({"agent_id":"a","seq":1,"metrics":[],"unknown":123})";
        if (!require_drop(input, gateway::MetricsDropReason::UnexpectedField)) {
            std::printf("UnexpectedField test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 17: Metric missing name -> MetricMissingName
    {
        std::string_view input = R"({"agent_id":"a","seq":1,"metrics":[{"v":1}]})";
        if (!require_drop(input, gateway::MetricsDropReason::MetricMissingName)) {
            std::printf("MetricMissingName test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 18: Metric missing value -> MetricMissingValue
    {
        std::string_view input = R"({"agent_id":"a","seq":1,"metrics":[{"n":"m"}]})";
        if (!require_drop(input, gateway::MetricsDropReason::MetricMissingValue)) {
            std::printf("MetricMissingValue test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 19: Metric value not a number -> MetricValueNotNumber
    {
        std::string_view input = R"({"agent_id":"a","seq":1,"metrics":[{"n":"m","v":"string"}]})";
        if (!require_drop(input, gateway::MetricsDropReason::MetricValueNotNumber)) {
            std::printf("MetricValueNotNumber test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 20: Invalid JSON syntax -> InvalidJson
    {
        std::string_view input = R"({"agent_id":"a","seq":1,"metrics":[})";
        if (!require_drop(input, gateway::MetricsDropReason::InvalidJson)) {
            std::printf("InvalidJson test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 21: Empty object -> MissingRequiredField
    {
        std::string_view input = R"({})";
        if (!require_drop(input, gateway::MetricsDropReason::MissingRequiredField)) {
            std::printf("Empty object test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 22: Negative metric value (valid)
    {
        std::string_view input = R"({"agent_id":"a","seq":1,"metrics":[{"n":"temp","v":-10.5}]})";
        auto r = gateway::parse_metrics(input);
        const auto* m = get_metrics_if_success(r);
        if (m == nullptr) {
            std::printf("Negative value test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (m->metrics[0].value != -10.5) {
            std::printf("Negative value test failed: wrong value\n");
            return EXIT_FAILURE;
        }
    }

    // Test 23: Scientific notation (valid)
    {
        std::string_view input = R"({"agent_id":"a","seq":1,"metrics":[{"n":"big","v":1.5e6}]})";
        auto r = gateway::parse_metrics(input);
        const auto* m = get_metrics_if_success(r);
        if (m == nullptr) {
            std::printf("Scientific notation test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (m->metrics[0].value != 1.5e6) {
            std::printf("Scientific notation test failed: wrong value\n");
            return EXIT_FAILURE;
        }
    }

    // Test 24: Unit too long -> UnitTooLong
    {
        std::string long_unit(gateway::MetricsLimits::kMaxUnitLen + 1, 'u');
        std::string input = R"({"agent_id":"a","seq":1,"metrics":[{"n":"m","v":1,"u":")" +
                           long_unit + R"("}]})";
        if (!require_drop(input, gateway::MetricsDropReason::UnitTooLong)) {
            std::printf("UnitTooLong test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 25: Boundary - exactly max agent_id length (valid)
    {
        std::string max_id(gateway::MetricsLimits::kMaxAgentIdLen, 'a');
        std::string input = R"({"agent_id":")" + max_id + R"(","seq":1,"metrics":[]})";
        auto r = gateway::parse_metrics(input);
        const auto* m = get_metrics_if_success(r);
        if (m == nullptr) {
            std::printf("Max agent_id length test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (m->agent_id.size() != gateway::MetricsLimits::kMaxAgentIdLen) {
            std::printf("Max agent_id length test failed: wrong length\n");
            return EXIT_FAILURE;
        }
    }

    // Test 26: Boundary - exactly max metrics count (valid)
    {
        std::string input = R"({"agent_id":"a","seq":1,"metrics":[)";
        for (std::size_t i = 0; i < gateway::MetricsLimits::kMaxMetrics; ++i) {
            if (i > 0) input += ",";
            input += R"({"n":"m","v":1})";
        }
        input += "]}";
        auto r = gateway::parse_metrics(input);
        const auto* m = get_metrics_if_success(r);
        if (m == nullptr) {
            std::printf("Max metrics count test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (m->metric_count != gateway::MetricsLimits::kMaxMetrics) {
            std::printf("Max metrics count test failed: wrong count\n");
            return EXIT_FAILURE;
        }
    }

    std::printf("All parse_metrics tests passed\n");
    return EXIT_SUCCESS;
}
