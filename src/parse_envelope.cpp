
#include "gateway/parse_envelope.hpp"

#include <cstddef>   // std::byte, std::to_integer
#include <cstdint>   // uint16_t
#include <limits>    // std::numeric_limits

namespace gateway {

ParseResult parse_envelope(std::span<const std::byte> payload) noexcept {
    // size > 1500 -> PayloadTooLarge
    if (payload.size() > kMaxDatagramBytes) {
        return DropReason::PayloadTooLarge;
    }

    // size < 2 -> PayloadTooSmall
    if (payload.size() < 2) {
        return DropReason::PayloadTooSmall;
    }

    // read claimed_len safely (network byte order: big-endian)
    const uint16_t b0 = std::to_integer<uint16_t>(payload[0]);
    const uint16_t b1 = std::to_integer<uint16_t>(payload[1]);
    const uint16_t claimed_len = static_cast<uint16_t>((b0 << 8) | b1);

    // compute expected_total = 2 + claimed_len (widen to avoid overflow surprises)
    const size_t expected_total = size_t{2} + static_cast<size_t>(claimed_len);

    // if expected_total > payload.size() -> LengthMismatch
    if (expected_total > payload.size()) {
        return DropReason::LengthMismatch;
    }

    // if expected_total < payload.size() -> TrailingJunk
    if (expected_total < payload.size()) {
        return DropReason::TrailingJunk;
    }

    // else return ParsedBody{payload.subspan(2, claimed_len)}
    return ParsedBody{ payload.subspan(2, static_cast<size_t>(claimed_len)) };
}

} // namespace gateway
