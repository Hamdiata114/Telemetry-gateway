#include "gateway/validate_log.hpp"
#include "gateway/parse_log.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <variant>

// TB-4 Log validation tests.
// Tests semantic validation of parsed logs.

namespace {

constexpr std::uint64_t kCurrentTime = 1705689600000;  // Fixed "now" for tests

bool is_validation_drop(const gateway::LogValidationResult& r,
                        gateway::LogValidationDrop reason) {
    if (const auto* dr = std::get_if<gateway::LogValidationDrop>(&r)) {
        return *dr == reason;
    }
    return false;
}

const gateway::ValidatedLog* get_validated(const gateway::LogValidationResult& r) {
    return std::get_if<gateway::ValidatedLog>(&r);
}

// Helper to parse and validate in one step
gateway::LogValidationResult parse_and_validate(
    std::string_view logfmt,
    const gateway::LogValidationConfig& config = gateway::kDefaultLogValidation,
    std::uint64_t current_time = kCurrentTime
) {
    auto parse_result = gateway::parse_log(logfmt);
    if (const auto* parsed = std::get_if<gateway::ParsedLog>(&parse_result)) {
        return gateway::validate_log(*parsed, config, current_time);
    }
    // Parse failed - this shouldn't happen in our tests
    std::printf("Parse failed unexpectedly\n");
    std::exit(EXIT_FAILURE);
}

}  // namespace

int main() {
    // =========================================================================
    // Success path tests
    // =========================================================================

    // Test 1: Valid log message
    {
        std::string log = "ts=" + std::to_string(kCurrentTime) + " level=info msg=hello";
        auto r = parse_and_validate(log);
        const auto* v = get_validated(r);
        if (v == nullptr) {
            std::printf("Test 1 failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (v->ts != kCurrentTime) {
            std::printf("Test 1 failed: wrong ts\n");
            return EXIT_FAILURE;
        }
        if (v->level != gateway::LogLevel::Info) {
            std::printf("Test 1 failed: wrong level\n");
            return EXIT_FAILURE;
        }
    }

    // Test 2: Valid with agent_id
    {
        std::string log = "ts=" + std::to_string(kCurrentTime) +
                         " level=error agent=NodeAlpha msg=failed";
        auto r = parse_and_validate(log);
        const auto* v = get_validated(r);
        if (v == nullptr) {
            std::printf("Test 2 failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (v->agent_id != "NodeAlpha") {
            std::printf("Test 2 failed: wrong agent_id\n");
            return EXIT_FAILURE;
        }
    }

    // Test 3: Valid timestamp at boundary (exactly 5 min ago)
    {
        std::uint64_t old_ts = kCurrentTime - 300'000;
        std::string log = "ts=" + std::to_string(old_ts) + " level=info msg=test";
        auto r = parse_and_validate(log);
        if (get_validated(r) == nullptr) {
            std::printf("Test 3 failed: timestamp at boundary should be valid\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // agent_id validation tests
    // =========================================================================

    // Test 4: agent_id starting with number -> invalid
    {
        std::string log = "ts=" + std::to_string(kCurrentTime) +
                         " level=info agent=1node msg=test";
        auto r = parse_and_validate(log);
        if (!is_validation_drop(r, gateway::LogValidationDrop::AgentIdInvalidFormat)) {
            std::printf("Test 4 failed: agent_id starting with number should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 5: agent_id with invalid characters -> invalid
    {
        std::string log = "ts=" + std::to_string(kCurrentTime) +
                         " level=info agent=node@host msg=test";
        auto r = parse_and_validate(log);
        if (!is_validation_drop(r, gateway::LogValidationDrop::AgentIdInvalidFormat)) {
            std::printf("Test 5 failed: agent_id with @ should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 6: Valid agent_id formats
    {
        const char* valid_ids[] = {"a", "Node", "node-1", "node_1", "NodeAlpha123"};
        for (const char* id : valid_ids) {
            std::string log = "ts=" + std::to_string(kCurrentTime) +
                             " level=info agent=" + std::string(id) + " msg=test";
            auto r = parse_and_validate(log);
            if (get_validated(r) == nullptr) {
                std::printf("Test 6 failed: agent_id '%s' should be valid\n", id);
                return EXIT_FAILURE;
            }
        }
    }

    // Test 7: Missing agent_id when not required -> valid
    {
        std::string log = "ts=" + std::to_string(kCurrentTime) + " level=info msg=test";
        gateway::LogValidationConfig config = gateway::kDefaultLogValidation;
        config.require_agent_id = false;

        auto r = parse_and_validate(log, config);
        if (get_validated(r) == nullptr) {
            std::printf("Test 7 failed: missing agent_id should be valid when not required\n");
            return EXIT_FAILURE;
        }
    }

    // Test 8: Missing agent_id when required -> invalid
    {
        std::string log = "ts=" + std::to_string(kCurrentTime) + " level=info msg=test";
        gateway::LogValidationConfig config = gateway::kDefaultLogValidation;
        config.require_agent_id = true;

        auto r = parse_and_validate(log, config);
        if (!is_validation_drop(r, gateway::LogValidationDrop::AgentIdEmpty)) {
            std::printf("Test 8 failed: missing agent_id should be rejected when required\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // Timestamp validation tests
    // =========================================================================

    // Test 9: Timestamp too old -> reject
    {
        std::uint64_t old_ts = kCurrentTime - 300'001;  // 1ms too old
        std::string log = "ts=" + std::to_string(old_ts) + " level=info msg=test";
        auto r = parse_and_validate(log);
        if (!is_validation_drop(r, gateway::LogValidationDrop::TimestampTooOld)) {
            std::printf("Test 9 failed: timestamp too old should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 10: Timestamp in future -> reject
    {
        std::uint64_t future_ts = kCurrentTime + 60'001;  // 1ms too far
        std::string log = "ts=" + std::to_string(future_ts) + " level=info msg=test";
        auto r = parse_and_validate(log);
        if (!is_validation_drop(r, gateway::LogValidationDrop::TimestampInFuture)) {
            std::printf("Test 10 failed: timestamp in future should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // Log level validation tests
    // =========================================================================

    // Test 11: All log levels with min_level=Trace -> all valid
    {
        gateway::LogValidationConfig config = gateway::kDefaultLogValidation;
        config.min_level = gateway::LogLevel::Trace;

        const char* levels[] = {"trace", "debug", "info", "warn", "error", "fatal"};
        for (const char* lvl : levels) {
            std::string log = "ts=" + std::to_string(kCurrentTime) +
                             " level=" + std::string(lvl) + " msg=test";
            auto r = parse_and_validate(log, config);
            if (get_validated(r) == nullptr) {
                std::printf("Test 11 failed: level '%s' should be valid\n", lvl);
                return EXIT_FAILURE;
            }
        }
    }

    // Test 12: Level below minimum -> reject
    {
        gateway::LogValidationConfig config = gateway::kDefaultLogValidation;
        config.min_level = gateway::LogLevel::Warn;

        std::string log = "ts=" + std::to_string(kCurrentTime) + " level=info msg=test";
        auto r = parse_and_validate(log, config);
        if (!is_validation_drop(r, gateway::LogValidationDrop::LevelBelowMinimum)) {
            std::printf("Test 12 failed: level below minimum should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 13: Level at minimum -> valid
    {
        gateway::LogValidationConfig config = gateway::kDefaultLogValidation;
        config.min_level = gateway::LogLevel::Warn;

        std::string log = "ts=" + std::to_string(kCurrentTime) + " level=warn msg=test";
        auto r = parse_and_validate(log, config);
        if (get_validated(r) == nullptr) {
            std::printf("Test 13 failed: level at minimum should be valid\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // Message validation tests
    // =========================================================================

    // Test 14: Empty message -> reject
    {
        // We need to construct a ParsedLog directly since the parser requires msg
        gateway::ParsedLog parsed{};
        parsed.ts = kCurrentTime;
        parsed.level = gateway::LogLevel::Info;
        parsed.msg = "";
        parsed.field_count = 0;

        auto r = gateway::validate_log(parsed, gateway::kDefaultLogValidation, kCurrentTime);
        if (!is_validation_drop(r, gateway::LogValidationDrop::MessageEmpty)) {
            std::printf("Test 14 failed: empty message should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 15: Message truncation when enabled
    {
        gateway::LogValidationConfig config = gateway::kDefaultLogValidation;
        config.max_message_length = 10;
        config.truncate_oversized_message = true;

        std::string log = "ts=" + std::to_string(kCurrentTime) +
                         " level=info msg=verylongmessagehere";
        auto r = parse_and_validate(log, config);
        const auto* v = get_validated(r);
        if (v == nullptr) {
            std::printf("Test 15 failed: oversized message should be truncated\n");
            return EXIT_FAILURE;
        }
        if (v->msg.size() != 10) {
            std::printf("Test 15 failed: message should be truncated to 10 chars, got %zu\n",
                       v->msg.size());
            return EXIT_FAILURE;
        }
    }

    // Test 16: Message rejection when truncation disabled
    {
        gateway::LogValidationConfig config = gateway::kDefaultLogValidation;
        config.max_message_length = 10;
        config.truncate_oversized_message = false;

        std::string log = "ts=" + std::to_string(kCurrentTime) +
                         " level=info msg=verylongmessagehere";
        auto r = parse_and_validate(log, config);
        if (!is_validation_drop(r, gateway::LogValidationDrop::MessageTooLong)) {
            std::printf("Test 16 failed: oversized message should be rejected\n");
            return EXIT_FAILURE;
        }
    }

    // Test 17: Message at exactly max length -> valid
    {
        gateway::LogValidationConfig config = gateway::kDefaultLogValidation;
        config.max_message_length = 10;
        config.truncate_oversized_message = false;

        std::string log = "ts=" + std::to_string(kCurrentTime) +
                         " level=info msg=exactly_10";  // exactly 10 chars
        auto r = parse_and_validate(log, config);
        if (get_validated(r) == nullptr) {
            std::printf("Test 17 failed: message at max length should be valid\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // Edge case tests
    // =========================================================================

    // Test 18: Quoted message with spaces
    {
        std::string log = "ts=" + std::to_string(kCurrentTime) +
                         R"( level=info msg="hello world")";
        auto r = parse_and_validate(log);
        const auto* v = get_validated(r);
        if (v == nullptr) {
            std::printf("Test 18 failed: quoted message should be valid\n");
            return EXIT_FAILURE;
        }
        if (v->msg != "hello world") {
            std::printf("Test 18 failed: wrong message content\n");
            return EXIT_FAILURE;
        }
    }

    // Test 19: Extra fields preserved
    {
        std::string log = "ts=" + std::to_string(kCurrentTime) +
                         " level=info msg=test host=server1 port=8080";
        auto r = parse_and_validate(log);
        const auto* v = get_validated(r);
        if (v == nullptr) {
            std::printf("Test 19 failed: extra fields should be valid\n");
            return EXIT_FAILURE;
        }
        if (v->field_count != 5) {  // ts, level, msg, host, port
            std::printf("Test 19 failed: wrong field count %zu\n", v->field_count);
            return EXIT_FAILURE;
        }
    }

    std::printf("All validate_log tests passed\n");
    return EXIT_SUCCESS;
}
