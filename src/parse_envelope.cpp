#include "gateway/parse_envelope.hpp"

namespace gateway {

ParseResult parse_envelope(std::span<const std::byte> payload) noexcept {
    // Stub behavior: always reject.
    // High-level purpose: verify build & linkage before adding real logic.
    (void)payload;
    return DropReason::PayloadTooSmall;
}

} // namespace gateway
