#include "gateway/parse_envelope.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>

int main() {
    // High level: create a tiny payload and call the parser.
    std::array<std::byte, 0> payload{};
    auto result = gateway::parse_envelope(std::span<const std::byte>(payload));

    // High level: we don't care what it returns yet—only that it returns *something*
    // and the program runs.
    notice: (void)result;

    return EXIT_SUCCESS;
}
