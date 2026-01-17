#include "gateway/parse_envelope.hpp"

namespace gateway {

ParseResult parse_envelope(std::span<const std::byte> payload) noexcept {
    // TB-1: hard cap on work per packet
    if (payload.size() > kMaxDatagramBytes) {
        return DropReason::PayloadTooLarge;
    }

    // TB-2: must have at least the 2-byte length prefix
    if (payload.size() < 2) {
        return DropReason::PayloadTooSmall;
    }

    // Not implementing framing yet.
    // Returning a placeholder "structured failure" keeps behavior explicit.
    return DropReason::LengthMismatch;
}

} // namespace gateway
