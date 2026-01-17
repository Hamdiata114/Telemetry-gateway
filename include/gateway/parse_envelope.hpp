#pragma once

#include <cstddef>   // std::byte, std::size_t
#include <cstdint>   // std::uint16_t
#include <span>      // std::span
#include <variant>   // std::variant

namespace gateway {

// Why: a finite taxonomy of "why we dropped" for tests/metrics.
enum class DropReason : std::uint8_t {
    PayloadTooLarge,
    PayloadTooSmall,
    LengthMismatch,   // declared body_len doesn't match available bytes
    TrailingJunk      // extra bytes beyond declared body_len
};

// Why: on success we return a bounded *view* into the original payload.
struct ParsedBody {
    std::span<const std::byte> body;
};

// Why: result is either success (ParsedBody) or failure (DropReason).
using ParseResult = std::variant<ParsedBody, DropReason>;

// High-level contract:
// - Takes untrusted bytes (payload)
// - Enforces hard bounds and framing
// - Never allocates, never throws
// - Returns either a safe view of the body or a drop reason
ParseResult parse_envelope(std::span<const std::byte> payload) noexcept;

// Your global TB-1 cap (explicit, testable).
inline constexpr std::size_t kMaxDatagramBytes = 1500;

} // namespace gateway
