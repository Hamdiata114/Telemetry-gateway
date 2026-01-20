#include "gateway/parse_log.hpp"

#include <cctype>
#include <charconv>
#include <cstring>
#include <optional>

namespace gateway {

namespace {

// Single-pass logfmt parser with bounded memory and CPU.
// Format: key=value key=value key="quoted value"
//
// Grammar:
//   line   = field (" " field)*
//   field  = key "=" value
//   key    = [a-z_][a-z0-9_]*
//   value  = bare | quoted
//   bare   = [^\s"=]+
//   quoted = '"' [^"]* '"'

class LogfmtParser {
public:
    explicit LogfmtParser(std::string_view input) noexcept
        : input_(input), pos_(0) {}

    LogResult parse() noexcept {
        // Invariant 1: Check size bound before any parsing
        if (input_.size() > LogLimits::kMaxLineBytes) {
            return LogDropReason::InputTooLarge;
        }

        if (input_.empty()) {
            return LogDropReason::EmptyInput;
        }

        // Strip trailing newline/whitespace
        while (!input_.empty() &&
               (input_.back() == '\n' || input_.back() == '\r' ||
                input_.back() == ' ' || input_.back() == '\t')) {
            input_ = input_.substr(0, input_.size() - 1);
        }

        if (input_.empty()) {
            return LogDropReason::EmptyInput;
        }

        ParsedLog result{};
        result.field_count = 0;
        result.ts = 0;
        result.level = LogLevel::Info;
        result.agent_id = {};
        result.msg = {};

        bool has_ts = false;
        bool has_level = false;
        bool has_msg = false;

        // Parse fields
        while (pos_ < input_.size()) {
            skip_spaces();
            if (pos_ >= input_.size()) break;

            // Invariant 2: Bound iteration count
            if (result.field_count >= LogLimits::kMaxFields) {
                return LogDropReason::TooManyFields;
            }

            // Parse key
            auto key = parse_key();
            if (!key) {
                return key.error();
            }

            if (key->size() > LogLimits::kMaxKeyLen) {
                return LogDropReason::KeyTooLong;
            }

            // Expect '='
            if (pos_ >= input_.size() || input_[pos_] != '=') {
                return LogDropReason::MissingEquals;
            }
            ++pos_; // consume '='

            // Parse value
            auto value = parse_value();
            if (!value) {
                return value.error();
            }

            if (value->size() > LogLimits::kMaxValueLen) {
                return LogDropReason::ValueTooLong;
            }

            // Store field
            result.fields[result.field_count].key = *key;
            result.fields[result.field_count].value = *value;
            ++result.field_count;

            // Handle known fields
            if (*key == "ts") {
                std::uint64_t ts_val = 0;
                auto [ptr, ec] = std::from_chars(value->data(), value->data() + value->size(), ts_val);
                if (ec != std::errc{} || ptr != value->data() + value->size()) {
                    return LogDropReason::InvalidTimestamp;
                }
                result.ts = ts_val;
                has_ts = true;
            } else if (*key == "level") {
                if (!parse_log_level(*value, result.level)) {
                    return LogDropReason::InvalidLevel;
                }
                has_level = true;
            } else if (*key == "msg") {
                result.msg = *value;
                has_msg = true;
            } else if (*key == "agent") {
                result.agent_id = *value;
            }
        }

        // Check required fields
        if (!has_ts) {
            return LogDropReason::MissingTimestamp;
        }
        if (!has_level) {
            return LogDropReason::MissingLevel;
        }
        if (!has_msg) {
            return LogDropReason::MissingMessage;
        }

        return result;
    }

private:
    std::string_view input_;
    std::size_t pos_;

    void skip_spaces() noexcept {
        while (pos_ < input_.size() &&
               (input_[pos_] == ' ' || input_[pos_] == '\t')) {
            ++pos_;
        }
    }

    // Result type for key/value parsing
    struct ParseError {
        LogDropReason reason;
    };

    template<typename T>
    struct Result {
        std::optional<T> value;
        LogDropReason error_reason;

        Result(T v) : value(std::move(v)), error_reason{} {}
        Result(LogDropReason e) : value(std::nullopt), error_reason(e) {}

        explicit operator bool() const { return value.has_value(); }
        T& operator*() { return *value; }
        const T& operator*() const { return *value; }
        T* operator->() { return &(*value); }
        const T* operator->() const { return &(*value); }
        LogDropReason error() const { return error_reason; }
    };

    // Parse a key: [a-z_][a-z0-9_]*
    Result<std::string_view> parse_key() noexcept {
        std::size_t start = pos_;

        // First character: [a-z_]
        if (pos_ >= input_.size()) {
            return LogDropReason::MissingEquals;
        }

        char c = input_[pos_];
        if (!is_key_start(c)) {
            return LogDropReason::InvalidKeyChar;
        }
        ++pos_;

        // Remaining characters: [a-z0-9_]
        while (pos_ < input_.size()) {
            c = input_[pos_];
            if (!is_key_char(c)) {
                break;
            }
            ++pos_;
        }

        return input_.substr(start, pos_ - start);
    }

    // Parse a value: bare or quoted
    Result<std::string_view> parse_value() noexcept {
        if (pos_ >= input_.size()) {
            // Empty value at end of line
            return std::string_view{};
        }

        if (input_[pos_] == '"') {
            return parse_quoted_value();
        } else {
            return parse_bare_value();
        }
    }

    // Parse bare value (unquoted): [^\s"=]+
    Result<std::string_view> parse_bare_value() noexcept {
        std::size_t start = pos_;

        while (pos_ < input_.size()) {
            char c = input_[pos_];
            if (c == ' ' || c == '\t' || c == '"' || c == '=') {
                break;
            }
            ++pos_;
        }

        return input_.substr(start, pos_ - start);
    }

    // Parse quoted value: "[^"]*"
    Result<std::string_view> parse_quoted_value() noexcept {
        if (pos_ >= input_.size() || input_[pos_] != '"') {
            return LogDropReason::UnterminatedQuote;
        }
        ++pos_; // consume opening quote

        std::size_t start = pos_;

        // Find closing quote (simple: no escape handling for logfmt)
        while (pos_ < input_.size()) {
            if (input_[pos_] == '"') {
                std::string_view result = input_.substr(start, pos_ - start);
                ++pos_; // consume closing quote
                return result;
            }
            ++pos_;
        }

        return LogDropReason::UnterminatedQuote;
    }

    static bool is_key_start(char c) noexcept {
        return (c >= 'a' && c <= 'z') || c == '_';
    }

    static bool is_key_char(char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    }
};

} // namespace

LogResult parse_log(std::span<const std::byte> input) noexcept {
    std::string_view sv(reinterpret_cast<const char*>(input.data()), input.size());
    return parse_log(sv);
}

LogResult parse_log(std::string_view input) noexcept {
    LogfmtParser parser(input);
    return parser.parse();
}

bool parse_log_level(std::string_view s, LogLevel& out) noexcept {
    if (s == "trace") { out = LogLevel::Trace; return true; }
    if (s == "debug") { out = LogLevel::Debug; return true; }
    if (s == "info")  { out = LogLevel::Info;  return true; }
    if (s == "warn")  { out = LogLevel::Warn;  return true; }
    if (s == "error") { out = LogLevel::Error; return true; }
    if (s == "fatal") { out = LogLevel::Fatal; return true; }
    return false;
}

} // namespace gateway
