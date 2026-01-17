#include "gateway/parse_envelope.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <variant>

namespace {

// High-level: check if result is a specific DropReason.
bool is_drop_reason(const gateway::ParseResult& r, gateway::DropReason reason) {
    if (const auto* dr = std::get_if<gateway::DropReason>(&r)) {
        return *dr == reason;
    }
    return false;
}

// High-level: write uint16 in network byte order (big-endian) into first two bytes.
void write_u16_be(std::span<std::byte> buf, std::uint16_t v) {
    buf[0] = std::byte((v >> 8) & 0xFF);
    buf[1] = std::byte(v & 0xFF);
}

// High-level: run one test and return true/false instead of exiting immediately.
bool require_drop(std::span<const std::byte> payload, gateway::DropReason expected) {
    auto r = gateway::parse_envelope(payload);
    return is_drop_reason(r, expected);
}

} // namespace

int main() {
    // Test 1: Oversized payload -> PayloadTooLarge
    {
        std::array<std::byte, gateway::kMaxDatagramBytes + 1> payload{};
        if (!require_drop(payload, gateway::DropReason::PayloadTooLarge)) {
            return EXIT_FAILURE;
        }
    }

    // Test 2: Too small to contain header -> PayloadTooSmall
    {
        std::array<std::byte, 1> payload{};
        if (!require_drop(payload, gateway::DropReason::PayloadTooSmall)) {
            return EXIT_FAILURE;
        }
    }

    // Test 3: Declared length > available bytes -> LengthMismatch
    // Header says body_len=10, but payload contains only 2 + 9 bytes total.
    {
        std::array<std::byte, 2 + 9> payload{};
        write_u16_be(std::span<std::byte>(payload).subspan(0, 2), 10);
        if (!require_drop(payload, gateway::DropReason::LengthMismatch)) {
            return EXIT_FAILURE;
        }
    }

    // Test 4: Extra bytes beyond declared body length -> TrailingJunk
    // Header says body_len=10, but payload contains 2 + 10 + 1 bytes total.
    {
        std::array<std::byte, 2 + 10 + 1> payload{};
        write_u16_be(std::span<std::byte>(payload).subspan(0, 2), 10);
        if (!require_drop(payload, gateway::DropReason::TrailingJunk)) {
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}
