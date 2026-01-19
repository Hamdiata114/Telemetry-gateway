#include "gateway/parse_envelope.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <variant>
#include <cstdio>

// TB-2 framing validation tests.
// Note: Size enforcement (PayloadTooLarge) is tested in TB-1 (RecvLoop) tests.

namespace {

// Check if result is a specific DropReason.
bool is_drop_reason(const gateway::ParseResult& r, gateway::DropReason reason) {
    if (const auto* dr = std::get_if<gateway::DropReason>(&r)) {
        return *dr == reason;
    }
    return false;
}

// Write uint16 in network byte order (big-endian) into first two bytes.
void write_u16_be(std::span<std::byte> buf, std::uint16_t v) {
    buf[0] = std::byte((v >> 8) & 0xFF);
    buf[1] = std::byte(v & 0xFF);
}

const gateway::ParsedBody* get_body_if_success(const gateway::ParseResult& r) {
    return std::get_if<gateway::ParsedBody>(&r);
}

// Run one test and return true/false.
bool require_drop(std::span<const std::byte> payload, gateway::DropReason expected) {
    auto r = gateway::parse_envelope(payload);
    return is_drop_reason(r, expected);
}

} // namespace

int main() {
    // Test 1: Too small to contain header -> PayloadTooSmall
    {
        std::array<std::byte, 1> payload{};
        if (!require_drop(payload, gateway::DropReason::PayloadTooSmall)) {
            std::printf("PayloadTooSmall test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 2: Declared length > available bytes -> LengthMismatch
    // Header says body_len=10, but payload contains only 2 + 9 bytes total.
    {
        std::array<std::byte, 2 + 9> payload{};
        write_u16_be(std::span<std::byte>(payload).subspan(0, 2), 10);
        if (!require_drop(payload, gateway::DropReason::LengthMismatch)) {
            std::printf("LengthMismatch test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 3: Extra bytes beyond declared body length -> TrailingJunk
    // Header says body_len=10, but payload contains 2 + 10 + 1 bytes total.
    {
        std::array<std::byte, 2 + 10 + 1> payload{};
        write_u16_be(std::span<std::byte>(payload).subspan(0, 2), 10);
        if (!require_drop(payload, gateway::DropReason::TrailingJunk)) {
            std::printf("TrailingJunk test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 4: Valid framing -> success with correct body span
    {
        constexpr std::uint16_t N = 10;
        std::array<std::byte, 2 + N> payload{};
        write_u16_be(std::span<std::byte>(payload).subspan(0, 2), N);

        // Fill body with a pattern to verify slicing is correct.
        for (std::size_t i = 0; i < N; ++i) {
            payload[2 + i] = std::byte(0xA0 + i);
        }

        auto r = gateway::parse_envelope(std::span<const std::byte>(payload));
        const auto* body = get_body_if_success(r);
        if (body == nullptr) {
            std::printf("Valid framing test failed: expected success\n");
            return EXIT_FAILURE;
        }

        if (body->body.size() != N) {
            std::printf("Valid framing test failed: wrong body length\n");
            return EXIT_FAILURE;
        }

        for (std::size_t i = 0; i < N; ++i) {
            if (body->body[i] != std::byte(0xA0 + i)) {
                std::printf("Valid framing test failed: body bytes mismatch\n");
                return EXIT_FAILURE;
            }
        }
    }

    std::printf("All parse_envelope tests passed\n");



    return EXIT_SUCCESS;
}
