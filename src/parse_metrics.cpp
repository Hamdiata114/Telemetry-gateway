#include "gateway/parse_metrics.hpp"

#include <cctype>
#include <charconv>
#include <cstring>
#include <optional>

namespace gateway {

namespace {

// Minimal JSON tokenizer with bounded parsing.
// Does NOT build a DOM - validates and extracts in single pass.

class JsonParser {
public:
    explicit JsonParser(std::string_view input) noexcept
        : input_(input), pos_(0), depth_(0) {}

    MetricsResult parse() noexcept {
        // Invariant 1: Check size bound before any parsing
        if (input_.size() > MetricsLimits::kMaxInputBytes) {
            return MetricsDropReason::InputTooLarge;
        }

        skip_whitespace();
        if (!expect('{')) {
            return MetricsDropReason::InvalidJson;
        }

        ParsedMetrics result{};
        result.metric_count = 0;
        result.ts = 0;

        bool has_agent_id = false;
        bool has_seq = false;
        bool has_metrics = false;

        // Parse root object fields
        skip_whitespace();
        if (peek() == '}') {
            advance();
            // Empty object - missing required fields
            return MetricsDropReason::MissingRequiredField;
        }

        while (true) {
            skip_whitespace();

            // Parse key
            auto key = parse_string();
            if (!key) {
                return MetricsDropReason::InvalidJson;
            }

            skip_whitespace();
            if (!expect(':')) {
                return MetricsDropReason::InvalidJson;
            }
            skip_whitespace();

            // Dispatch based on key
            if (*key == "agent_id") {
                auto val = parse_string();
                if (!val) {
                    return MetricsDropReason::InvalidFieldType;
                }
                if (val->size() > MetricsLimits::kMaxAgentIdLen) {
                    return MetricsDropReason::AgentIdTooLong;
                }
                if (!validate_agent_id(*val)) {
                    return MetricsDropReason::AgentIdInvalidChars;
                }
                result.agent_id = *val;
                has_agent_id = true;
            } else if (*key == "seq") {
                auto val = parse_integer();
                if (!val) {
                    return MetricsDropReason::InvalidFieldType;
                }
                result.seq = static_cast<std::uint32_t>(*val);
                has_seq = true;
            } else if (*key == "ts") {
                auto val = parse_integer();
                if (!val) {
                    return MetricsDropReason::InvalidFieldType;
                }
                result.ts = static_cast<std::uint64_t>(*val);
            } else if (*key == "metrics") {
                auto r = parse_metrics_array(result);
                if (r) {
                    return *r;
                }
                has_metrics = true;
            } else {
                // additionalProperties: false
                return MetricsDropReason::UnexpectedField;
            }

            skip_whitespace();
            if (peek() == '}') {
                advance();
                break;
            }
            if (!expect(',')) {
                return MetricsDropReason::InvalidJson;
            }
        }

        // Check required fields
        if (!has_agent_id || !has_seq || !has_metrics) {
            return MetricsDropReason::MissingRequiredField;
        }

        return result;
    }

private:
    std::string_view input_;
    std::size_t pos_;
    std::size_t depth_;

    char peek() const noexcept {
        return (pos_ < input_.size()) ? input_[pos_] : '\0';
    }

    char advance() noexcept {
        return (pos_ < input_.size()) ? input_[pos_++] : '\0';
    }

    bool expect(char c) noexcept {
        if (peek() == c) {
            advance();
            return true;
        }
        return false;
    }

    void skip_whitespace() noexcept {
        while (pos_ < input_.size()) {
            char c = input_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    // Parse a JSON string, returns view into original input
    std::optional<std::string_view> parse_string() noexcept {
        if (!expect('"')) {
            return std::nullopt;
        }

        std::size_t start = pos_;
        while (pos_ < input_.size()) {
            char c = input_[pos_];
            if (c == '"') {
                std::string_view result = input_.substr(start, pos_ - start);
                advance(); // consume closing quote
                return result;
            }
            if (c == '\\') {
                // Skip escaped character
                advance();
                if (pos_ < input_.size()) {
                    advance();
                }
            } else {
                advance();
            }
        }
        return std::nullopt; // Unterminated string
    }

    // Parse a JSON integer
    std::optional<std::int64_t> parse_integer() noexcept {
        std::size_t start = pos_;

        // Handle optional negative sign
        if (peek() == '-') {
            advance();
        }

        if (!std::isdigit(static_cast<unsigned char>(peek()))) {
            return std::nullopt;
        }

        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }

        std::string_view num_str = input_.substr(start, pos_ - start);
        std::int64_t value = 0;
        auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), value);
        if (ec != std::errc{}) {
            return std::nullopt;
        }
        return value;
    }

    // Parse a JSON number (integer or floating point)
    std::optional<double> parse_number() noexcept {
        std::size_t start = pos_;

        // Sign
        if (peek() == '-') {
            advance();
        }

        // Integer part
        if (!std::isdigit(static_cast<unsigned char>(peek()))) {
            return std::nullopt;
        }
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }

        // Fractional part
        if (peek() == '.') {
            advance();
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }

        // Exponent
        if (peek() == 'e' || peek() == 'E') {
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }

        std::string_view num_str = input_.substr(start, pos_ - start);
        double value = 0.0;
        auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), value);
        if (ec != std::errc{}) {
            return std::nullopt;
        }
        return value;
    }

    // Validate agent_id characters: ^[a-zA-Z0-9_.-]+$
    static bool validate_agent_id(std::string_view s) noexcept {
        if (s.empty()) return false;
        for (char c : s) {
            if (!std::isalnum(static_cast<unsigned char>(c)) &&
                c != '_' && c != '.' && c != '-') {
                return false;
            }
        }
        return true;
    }

    // Skip a JSON value (for unknown fields - but we reject those)
    bool skip_value() noexcept {
        skip_whitespace();
        char c = peek();

        if (c == '"') {
            return parse_string().has_value();
        } else if (c == '{') {
            return skip_object();
        } else if (c == '[') {
            return skip_array();
        } else if (c == 't' || c == 'f') {
            return skip_literal();
        } else if (c == 'n') {
            return skip_literal();
        } else if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parse_number().has_value();
        }
        return false;
    }

    bool skip_object() noexcept {
        if (!expect('{')) return false;
        if (++depth_ > MetricsLimits::kMaxNestingDepth) return false;

        skip_whitespace();
        if (peek() == '}') {
            advance();
            --depth_;
            return true;
        }

        while (true) {
            skip_whitespace();
            if (!parse_string()) return false;
            skip_whitespace();
            if (!expect(':')) return false;
            if (!skip_value()) return false;
            skip_whitespace();
            if (peek() == '}') {
                advance();
                --depth_;
                return true;
            }
            if (!expect(',')) return false;
        }
    }

    bool skip_array() noexcept {
        if (!expect('[')) return false;
        if (++depth_ > MetricsLimits::kMaxNestingDepth) return false;

        skip_whitespace();
        if (peek() == ']') {
            advance();
            --depth_;
            return true;
        }

        while (true) {
            if (!skip_value()) return false;
            skip_whitespace();
            if (peek() == ']') {
                advance();
                --depth_;
                return true;
            }
            if (!expect(',')) return false;
        }
    }

    bool skip_literal() noexcept {
        // true, false, null
        if (input_.substr(pos_).starts_with("true")) {
            pos_ += 4;
            return true;
        }
        if (input_.substr(pos_).starts_with("false")) {
            pos_ += 5;
            return true;
        }
        if (input_.substr(pos_).starts_with("null")) {
            pos_ += 4;
            return true;
        }
        return false;
    }

    // Parse the metrics array
    std::optional<MetricsDropReason> parse_metrics_array(ParsedMetrics& result) noexcept {
        if (!expect('[')) {
            return MetricsDropReason::InvalidFieldType;
        }
        if (++depth_ > MetricsLimits::kMaxNestingDepth) {
            return MetricsDropReason::NestingTooDeep;
        }

        skip_whitespace();
        if (peek() == ']') {
            advance();
            --depth_;
            result.metric_count = 0;
            return std::nullopt; // Empty array is valid
        }

        while (true) {
            // Invariant 2: Bound iteration count
            if (result.metric_count >= MetricsLimits::kMaxMetrics) {
                return MetricsDropReason::TooManyMetrics;
            }

            auto r = parse_metric(result.metrics[result.metric_count]);
            if (r) {
                return r;
            }
            ++result.metric_count;

            skip_whitespace();
            if (peek() == ']') {
                advance();
                --depth_;
                return std::nullopt;
            }
            if (!expect(',')) {
                return MetricsDropReason::InvalidJson;
            }
            skip_whitespace();
        }
    }

    // Parse a single metric object
    std::optional<MetricsDropReason> parse_metric(Metric& metric) noexcept {
        if (!expect('{')) {
            return MetricsDropReason::InvalidJson;
        }
        if (++depth_ > MetricsLimits::kMaxNestingDepth) {
            return MetricsDropReason::NestingTooDeep;
        }

        metric.name = {};
        metric.value = 0.0;
        metric.unit = {};
        metric.tag_count = 0;

        bool has_name = false;
        bool has_value = false;

        skip_whitespace();
        if (peek() == '}') {
            advance();
            --depth_;
            return MetricsDropReason::MetricMissingName;
        }

        while (true) {
            skip_whitespace();

            auto key = parse_string();
            if (!key) {
                return MetricsDropReason::InvalidJson;
            }

            skip_whitespace();
            if (!expect(':')) {
                return MetricsDropReason::InvalidJson;
            }
            skip_whitespace();

            if (*key == "n") {
                auto val = parse_string();
                if (!val) {
                    return MetricsDropReason::InvalidFieldType;
                }
                if (val->size() > MetricsLimits::kMaxMetricNameLen) {
                    return MetricsDropReason::MetricNameTooLong;
                }
                metric.name = *val;
                has_name = true;
            } else if (*key == "v") {
                auto val = parse_number();
                if (!val) {
                    return MetricsDropReason::MetricValueNotNumber;
                }
                metric.value = *val;
                has_value = true;
            } else if (*key == "u") {
                auto val = parse_string();
                if (!val) {
                    return MetricsDropReason::InvalidFieldType;
                }
                if (val->size() > MetricsLimits::kMaxUnitLen) {
                    return MetricsDropReason::UnitTooLong;
                }
                metric.unit = *val;
            } else if (*key == "t") {
                auto r = parse_tags(metric);
                if (r) {
                    return r;
                }
            } else {
                // additionalProperties: false
                return MetricsDropReason::UnexpectedField;
            }

            skip_whitespace();
            if (peek() == '}') {
                advance();
                --depth_;
                break;
            }
            if (!expect(',')) {
                return MetricsDropReason::InvalidJson;
            }
        }

        if (!has_name) {
            return MetricsDropReason::MetricMissingName;
        }
        if (!has_value) {
            return MetricsDropReason::MetricMissingValue;
        }

        return std::nullopt;
    }

    // Parse tags object
    std::optional<MetricsDropReason> parse_tags(Metric& metric) noexcept {
        if (!expect('{')) {
            return MetricsDropReason::InvalidFieldType;
        }
        if (++depth_ > MetricsLimits::kMaxNestingDepth) {
            return MetricsDropReason::NestingTooDeep;
        }

        skip_whitespace();
        if (peek() == '}') {
            advance();
            --depth_;
            return std::nullopt; // Empty tags
        }

        while (true) {
            // Invariant 2: Bound iteration count
            if (metric.tag_count >= MetricsLimits::kMaxTags) {
                return MetricsDropReason::TooManyTags;
            }

            skip_whitespace();
            auto key = parse_string();
            if (!key) {
                return MetricsDropReason::InvalidJson;
            }
            if (key->size() > MetricsLimits::kMaxTagKeyLen) {
                return MetricsDropReason::TagKeyTooLong;
            }

            skip_whitespace();
            if (!expect(':')) {
                return MetricsDropReason::InvalidJson;
            }
            skip_whitespace();

            auto val = parse_string();
            if (!val) {
                return MetricsDropReason::InvalidFieldType;
            }
            if (val->size() > MetricsLimits::kMaxTagValueLen) {
                return MetricsDropReason::TagValueTooLong;
            }

            metric.tags[metric.tag_count].key = *key;
            metric.tags[metric.tag_count].value = *val;
            ++metric.tag_count;

            skip_whitespace();
            if (peek() == '}') {
                advance();
                --depth_;
                return std::nullopt;
            }
            if (!expect(',')) {
                return MetricsDropReason::InvalidJson;
            }
        }
    }
};

} // namespace

MetricsResult parse_metrics(std::span<const std::byte> input) noexcept {
    std::string_view sv(reinterpret_cast<const char*>(input.data()), input.size());
    return parse_metrics(sv);
}

MetricsResult parse_metrics(std::string_view input) noexcept {
    JsonParser parser(input);
    return parser.parse();
}

} // namespace gateway
