#include "gateway/validate_config.hpp"

namespace gateway {

bool validate_agent_id_format(const char* data, std::size_t len) noexcept {
    // Format: ^[a-zA-Z][a-zA-Z0-9_-]{0,63}$
    // - Must start with letter
    // - Remaining: letters, digits, underscore, hyphen
    // - Length: 1-64 characters

    // Check length bounds
    if (len < AgentIdRules::kMinLength || len > AgentIdRules::kMaxLength) {
        return false;
    }

    // First character must be a letter
    char c = data[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        return false;
    }

    // Remaining characters: [a-zA-Z0-9_-]
    for (std::size_t i = 1; i < len; ++i) {
        c = data[i];
        bool valid = (c >= 'a' && c <= 'z') ||
                     (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') ||
                     c == '_' || c == '-';
        if (!valid) {
            return false;
        }
    }

    return true;
}

bool validate_timestamp_window(
    std::uint64_t ts,
    std::uint64_t current_time_ms,
    const TimestampWindow& window
) noexcept {
    // Calculate bounds (handle underflow for min_allowed)
    std::uint64_t min_allowed = 0;
    if (current_time_ms > static_cast<std::uint64_t>(window.max_age_ms)) {
        min_allowed = current_time_ms - static_cast<std::uint64_t>(window.max_age_ms);
    }

    std::uint64_t max_allowed = current_time_ms + static_cast<std::uint64_t>(window.max_future_ms);

    return ts >= min_allowed && ts <= max_allowed;
}

}  // namespace gateway
