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

    // Test 5: Zero-length body (body_len=0) -> valid with empty body
    {
        std::array<std::byte, 2> payload{};
        write_u16_be(std::span<std::byte>(payload).subspan(0, 2), 0);  // body_len = 0

        auto r = gateway::parse_envelope(std::span<const std::byte>(payload));
        const auto* body = get_body_if_success(r);
        if (body == nullptr) {
            std::printf("Zero-length body test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (!body->body.empty()) {
            std::printf("Zero-length body test failed: expected empty body, got %zu bytes\n", body->body.size());
            return EXIT_FAILURE;
        }
    }

    // Test 6: Max uint16 body_len (0xFFFF) with insufficient data -> LengthMismatch
    {
        std::array<std::byte, 2 + 100> payload{};  // Only 100 bytes of body, not 65535
        write_u16_be(std::span<std::byte>(payload).subspan(0, 2), 0xFFFF);

        if (!require_drop(payload, gateway::DropReason::LengthMismatch)) {
            std::printf("Max uint16 LengthMismatch test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 7: body_len=1 with exactly 1 byte body -> valid
    {
        std::array<std::byte, 2 + 1> payload{};
        write_u16_be(std::span<std::byte>(payload).subspan(0, 2), 1);
        payload[2] = std::byte(0xAB);

        auto r = gateway::parse_envelope(std::span<const std::byte>(payload));
        const auto* body = get_body_if_success(r);
        if (body == nullptr) {
            std::printf("Single byte body test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (body->body.size() != 1 || body->body[0] != std::byte(0xAB)) {
            std::printf("Single byte body test failed: wrong body content\n");
            return EXIT_FAILURE;
        }
    }

    // Test 8: Empty payload (0 bytes) -> PayloadTooSmall
    {
        std::span<const std::byte> empty_payload;
        if (!require_drop(empty_payload, gateway::DropReason::PayloadTooSmall)) {
            std::printf("Empty payload test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 9: Off-by-one short (body_len=10, but only 9 bytes available)
    {
        std::array<std::byte, 2 + 9> payload{};
        write_u16_be(std::span<std::byte>(payload).subspan(0, 2), 10);  // Claims 10, has 9

        if (!require_drop(payload, gateway::DropReason::LengthMismatch)) {
            std::printf("Off-by-one short test failed\n");
            return EXIT_FAILURE;
        }
    }

    // Test 10: Byte order verification - ensure big-endian parsing
    // 0x01 0x00 should be interpreted as body_len=256, not 1
    {
        std::array<std::byte, 2 + 256> payload{};
        payload[0] = std::byte(0x01);
        payload[1] = std::byte(0x00);

        auto r = gateway::parse_envelope(std::span<const std::byte>(payload));
        const auto* body = get_body_if_success(r);
        if (body == nullptr) {
            std::printf("Byte order test failed: expected success\n");
            return EXIT_FAILURE;
        }
        if (body->body.size() != 256) {
            std::printf("Byte order test failed: expected 256 bytes, got %zu\n", body->body.size());
            return EXIT_FAILURE;
        }
    }

    std::printf("All parse_envelope tests passed\n");

    return EXIT_SUCCESS;
}
