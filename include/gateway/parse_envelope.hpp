#pragma once

#include <cstddef>   // std::byte, std::size_t
#include <cstdint>   // std::uint16_t
#include <span>      // std::span
#include <variant>   // std::variant

namespace gateway {

// TB-2 drop reasons: framing validation failures.
// Note: Size enforcement (PayloadTooLarge) is handled at TB-1 (RecvLoop).
enum class DropReason : std::uint8_t {
    PayloadTooSmall,  // < 2 bytes, can't read header
    LengthMismatch,   // declared body_len > available bytes
    TrailingJunk      // extra bytes beyond declared body_len
};

// On success we return a bounded *view* into the original payload.
struct ParsedBody {
    std::span<const std::byte> body;
};

// Result is either success (ParsedBody) or failure (DropReason).
using ParseResult = std::variant<ParsedBody, DropReason>;

// TB-2: Envelope framing validation.
//
// Precondition: payload size already enforced by TB-1 (RecvLoop).
//
// Contract:
// - Validates framing (2-byte length header + body)
// - Never allocates, never throws
// - Returns either a safe view of the body or a drop reason
ParseResult parse_envelope(std::span<const std::byte> payload) noexcept;

} // namespace gateway
