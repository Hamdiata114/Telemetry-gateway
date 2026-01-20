#include "gateway/parse_log.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <variant>

// TB-3 logfmt log parsing tests.
// Tests both invariants:
// 1. Bounded memory allocation
// 2. Bounded CPU (parsing work)

namespace {

bool is_drop_reason(const gateway::LogResult& r, gateway::LogDropReason reason) {
    if (const auto* dr = std::get_if<gateway::LogDropReason>(&r)) {
        return *dr == reason;
    }
    return false;
}

const gateway::ParsedLog* get_log_if_success(const gateway::LogResult& r) {
    return std::get_if<gateway::ParsedLog>(&r);
}

bool require_drop(std::string_view input, gateway::LogDropReason expected) {
    auto r = gateway::parse_log(input);
    return is_drop_reason(r, expected);
}

} // namespace

int main() {
    // =========================================================================
    // Success path tests
    // =========================================================================

    // Test 1: Minimal valid log
    {
        std::string_view input = "ts=1705689600000 level=info msg=hello";
        auto r = gateway::parse_log(input);
        const auto* log = get_log_if_success(r);
        if (log == nullptr) {
            std::printf("Minimal valid log test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (log->ts != 1705689600000) {
            std::printf("Minimal valid log test failed: wrong ts\n");
            return EXIT_FAILURE;
        }
        if (log->level != gateway::LogLevel::Info) {
            std::printf("Minimal valid log test failed: wrong level\n");
            return EXIT_FAILURE;
        }
        if (log->msg != "hello") {
            std::printf("Minimal valid log test failed: wrong msg\n");
            return EXIT_FAILURE;
        }
    }

    // Test 2: Full log with agent and quoted message
    {
        std::string_view input = R"(ts=1705689600000 level=error agent=node-42 msg="Connection refused")";
        auto r = gateway::parse_log(input);
        const auto* log = get_log_if_success(r);
        if (log == nullptr) {
            std::printf("Full log test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (log->ts != 1705689600000) {
            std::printf("Full log test failed: wrong ts\n");
            return EXIT_FAILURE;
        }
        if (log->level != gateway::LogLevel::Error) {
            std::printf("Full log test failed: wrong level\n");
            return EXIT_FAILURE;
        }
        if (log->agent_id != "node-42") {
            std::printf("Full log test failed: wrong agent_id\n");
            return EXIT_FAILURE;
        }
        if (log->msg != "Connection refused") {
            std::printf("Full log test failed: wrong msg, got '%.*s'\n",
                       static_cast<int>(log->msg.size()), log->msg.data());
            return EXIT_FAILURE;
        }
    }

    // Test 3: All log levels
    {
        const char* levels[] = {"trace", "debug", "info", "warn", "error", "fatal"};
        gateway::LogLevel expected[] = {
            gateway::LogLevel::Trace,
            gateway::LogLevel::Debug,
            gateway::LogLevel::Info,
            gateway::LogLevel::Warn,
            gateway::LogLevel::Error,
            gateway::LogLevel::Fatal
        };

        for (int i = 0; i < 6; ++i) {
            std::string input = "ts=1 level=";
            input += levels[i];
            input += " msg=test";

            auto r = gateway::parse_log(input);
            const auto* log = get_log_if_success(r);
            if (log == nullptr) {
                std::printf("Log level '%s' test failed: expected success\n", levels[i]);
                return EXIT_FAILURE;
            }
            if (log->level != expected[i]) {
                std::printf("Log level '%s' test failed: wrong level\n", levels[i]);
                return EXIT_FAILURE;
            }
        }
    }

    // Test 4: Extra fields preserved
    {
        std::string_view input = "ts=1 level=info msg=test host=db-1 port=5432";
        auto r = gateway::parse_log(input);
        const auto* log = get_log_if_success(r);
        if (log == nullptr) {
            std::printf("Extra fields test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (log->field_count != 5) {
            std::printf("Extra fields test failed: wrong field count %zu\n", log->field_count);
            return EXIT_FAILURE;
        }
    }

    // Test 5: Quoted value with spaces
    {
        std::string_view input = R"(ts=1 level=info msg="hello world with spaces")";
        auto r = gateway::parse_log(input);
        const auto* log = get_log_if_success(r);
        if (log == nullptr) {
            std::printf("Quoted value test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (log->msg != "hello world with spaces") {
            std::printf("Quoted value test failed: wrong msg\n");
            return EXIT_FAILURE;
        }
    }

    // Test 6: Trailing newline stripped
    {
        std::string_view input = "ts=1 level=info msg=test\n";
        auto r = gateway::parse_log(input);
        const auto* log = get_log_if_success(r);
        if (log == nullptr) {
            std::printf("Trailing newline test failed: expected success\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // Invariant 1: Bounded memory allocation tests
    // =========================================================================

    // Test 7: Input too large -> InputTooLarge
    {
        std::string large_input(gateway::LogLimits::kMaxLineBytes + 1, 'a');
        if (!require_drop(large_input, gateway::LogDropReason::InputTooLarge)) {
            std::printf("InputTooLarge test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 8: Key too long -> KeyTooLong
    {
        std::string long_key(gateway::LogLimits::kMaxKeyLen + 1, 'k');
        std::string input = "ts=1 level=info msg=test " + long_key + "=value";
        if (!require_drop(input, gateway::LogDropReason::KeyTooLong)) {
            std::printf("KeyTooLong test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 9: Value too long -> ValueTooLong
    {
        std::string long_value(gateway::LogLimits::kMaxValueLen + 1, 'v');
        std::string input = "ts=1 level=info msg=" + long_value;
        if (!require_drop(input, gateway::LogDropReason::ValueTooLong)) {
            std::printf("ValueTooLong test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 10: Too many fields -> TooManyFields
    {
        std::string input = "ts=1 level=info msg=test";
        for (std::size_t i = 0; i < gateway::LogLimits::kMaxFields; ++i) {
            input += " f" + std::to_string(i) + "=v";
        }
        if (!require_drop(input, gateway::LogDropReason::TooManyFields)) {
            std::printf("TooManyFields test failed\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // Schema validation tests
    // =========================================================================

    // Test 11: Empty input -> EmptyInput
    {
        std::string_view input = "";
        if (!require_drop(input, gateway::LogDropReason::EmptyInput)) {
            std::printf("EmptyInput test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 12: Whitespace only -> EmptyInput
    {
        std::string_view input = "   \t\n";
        if (!require_drop(input, gateway::LogDropReason::EmptyInput)) {
            std::printf("Whitespace only test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 13: Missing timestamp -> MissingTimestamp
    {
        std::string_view input = "level=info msg=test";
        if (!require_drop(input, gateway::LogDropReason::MissingTimestamp)) {
            std::printf("MissingTimestamp test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 14: Missing level -> MissingLevel
    {
        std::string_view input = "ts=1 msg=test";
        if (!require_drop(input, gateway::LogDropReason::MissingLevel)) {
            std::printf("MissingLevel test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 15: Missing message -> MissingMessage
    {
        std::string_view input = "ts=1 level=info";
        if (!require_drop(input, gateway::LogDropReason::MissingMessage)) {
            std::printf("MissingMessage test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 16: Invalid timestamp -> InvalidTimestamp
    {
        std::string_view input = "ts=notanumber level=info msg=test";
        if (!require_drop(input, gateway::LogDropReason::InvalidTimestamp)) {
            std::printf("InvalidTimestamp test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 17: Invalid level -> InvalidLevel
    {
        std::string_view input = "ts=1 level=unknown msg=test";
        if (!require_drop(input, gateway::LogDropReason::InvalidLevel)) {
            std::printf("InvalidLevel test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 18: Invalid key character -> InvalidKeyChar
    {
        std::string_view input = "ts=1 level=info msg=test Bad_Key=value";
        if (!require_drop(input, gateway::LogDropReason::InvalidKeyChar)) {
            std::printf("InvalidKeyChar test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 19: Missing equals -> MissingEquals
    {
        std::string_view input = "ts=1 level=info msg test";
        if (!require_drop(input, gateway::LogDropReason::MissingEquals)) {
            std::printf("MissingEquals test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 20: Unterminated quote -> UnterminatedQuote
    {
        std::string_view input = R"(ts=1 level=info msg="unterminated)";
        if (!require_drop(input, gateway::LogDropReason::UnterminatedQuote)) {
            std::printf("UnterminatedQuote test failed\n");
            return EXIT_FAILURE;
        }
    }

    // =========================================================================
    // Boundary tests
    // =========================================================================

    // Test 21: Boundary - exactly max line bytes (valid)
    {
        // Build a valid log that's exactly at the limit
        std::string input = "ts=1 level=info msg=";
        std::size_t remaining = gateway::LogLimits::kMaxLineBytes - input.size();
        if (remaining > gateway::LogLimits::kMaxValueLen) {
            remaining = gateway::LogLimits::kMaxValueLen;
        }
        input += std::string(remaining, 'x');

        auto r = gateway::parse_log(input);
        const auto* log = get_log_if_success(r);
        if (log == nullptr) {
            std::printf("Max line bytes boundary test failed: expected success\n");
            return EXIT_FAILURE;
        }
    }

    // Test 22: Boundary - exactly max fields (valid)
    {
        std::string input = "ts=1 level=info msg=test";
        // Already have 3 fields, add up to max
        for (std::size_t i = 3; i < gateway::LogLimits::kMaxFields; ++i) {
            input += " f" + std::to_string(i) + "=v";
        }
        auto r = gateway::parse_log(input);
        const auto* log = get_log_if_success(r);
        if (log == nullptr) {
            std::printf("Max fields boundary test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (log->field_count != gateway::LogLimits::kMaxFields) {
            std::printf("Max fields boundary test failed: wrong count %zu\n", log->field_count);
            return EXIT_FAILURE;
        }
    }

    // Test 23: Empty quoted string (valid)
    {
        std::string_view input = R"(ts=1 level=info msg="")";
        auto r = gateway::parse_log(input);
        const auto* log = get_log_if_success(r);
        if (log == nullptr) {
            std::printf("Empty quoted string test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (!log->msg.empty()) {
            std::printf("Empty quoted string test failed: msg should be empty\n");
            return EXIT_FAILURE;
        }
    }

    // Test 24: log_level_to_string roundtrip
    {
        using gateway::LogLevel;
        using gateway::log_level_to_string;

        if (log_level_to_string(LogLevel::Trace) != "trace") {
            std::printf("log_level_to_string(Trace) failed\n");
            return EXIT_FAILURE;
        }
        if (log_level_to_string(LogLevel::Debug) != "debug") {
            std::printf("log_level_to_string(Debug) failed\n");
            return EXIT_FAILURE;
        }
        if (log_level_to_string(LogLevel::Info) != "info") {
            std::printf("log_level_to_string(Info) failed\n");
            return EXIT_FAILURE;
        }
        if (log_level_to_string(LogLevel::Warn) != "warn") {
            std::printf("log_level_to_string(Warn) failed\n");
            return EXIT_FAILURE;
        }
        if (log_level_to_string(LogLevel::Error) != "error") {
            std::printf("log_level_to_string(Error) failed\n");
            return EXIT_FAILURE;
        }
        if (log_level_to_string(LogLevel::Fatal) != "fatal") {
            std::printf("log_level_to_string(Fatal) failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 25: Multiple tabs/spaces between fields
    {
        std::string_view input = "ts=1   level=info\t\tmsg=test";
        auto r = gateway::parse_log(input);
        const auto* log = get_log_if_success(r);
        if (log == nullptr) {
            std::printf("Multiple whitespace test failed: expected success\n");
            return EXIT_FAILURE;
        }
    }

    std::printf("All parse_log tests passed\n");
    return EXIT_SUCCESS;
}
