#include "gateway/validate_log.hpp"

namespace gateway {

LogValidationResult validate_log(
    const ParsedLog& parsed,
    const LogValidationConfig& config,
    std::uint64_t current_time_ms
) noexcept {
    // =========================================================================
    // Validate agent_id (if provided or required)
    // =========================================================================

    if (!parsed.agent_id.empty()) {
        // Check length
        if (parsed.agent_id.size() > AgentIdRules::kMaxLength) {
            return LogValidationDrop::AgentIdTooLong;
        }

        // Check format: ^[a-zA-Z][a-zA-Z0-9_-]{0,63}$
        if (!validate_agent_id_format(parsed.agent_id.data(), parsed.agent_id.size())) {
            return LogValidationDrop::AgentIdInvalidFormat;
        }
    } else if (config.require_agent_id) {
        return LogValidationDrop::AgentIdEmpty;
    }

    // =========================================================================
    // Validate timestamp
    // =========================================================================

    if (!validate_timestamp_window(parsed.ts, current_time_ms, config.timestamp_window)) {
        // Determine if too old or in future
        std::uint64_t min_allowed = 0;
        if (current_time_ms > static_cast<std::uint64_t>(config.timestamp_window.max_age_ms)) {
            min_allowed = current_time_ms - static_cast<std::uint64_t>(config.timestamp_window.max_age_ms);
        }

        if (parsed.ts < min_allowed) {
            return LogValidationDrop::TimestampTooOld;
        } else {
            return LogValidationDrop::TimestampInFuture;
        }
    }

    // =========================================================================
    // Validate log level
    // =========================================================================

    if (static_cast<std::uint8_t>(parsed.level) < static_cast<std::uint8_t>(config.min_level)) {
        return LogValidationDrop::LevelBelowMinimum;
    }

    // =========================================================================
    // Validate message
    // =========================================================================

    if (parsed.msg.empty()) {
        return LogValidationDrop::MessageEmpty;
    }

    // Check message length
    std::string_view final_msg = parsed.msg;
    if (parsed.msg.size() > config.max_message_length) {
        if (config.truncate_oversized_message) {
            // Truncate message
            final_msg = parsed.msg.substr(0, config.max_message_length);
        } else {
            return LogValidationDrop::MessageTooLong;
        }
    }

    // =========================================================================
    // All validations passed - return validated log
    // =========================================================================

    ValidatedLog result;
    result.agent_id = parsed.agent_id;
    result.ts = parsed.ts;
    result.level = parsed.level;
    result.msg = final_msg;
    result.fields = parsed.fields.data();
    result.field_count = parsed.field_count;

    return result;
}

}  // namespace gateway
