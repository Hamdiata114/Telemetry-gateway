#include "gateway/recv_loop.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// Platform detection: MSG_TRUNC behavior differs between Linux and macOS
// On Linux, recvfrom with MSG_TRUNC returns the actual packet size even if truncated
// On macOS, it just returns the truncated size, so truncation detection doesn't work
#if defined(__linux__)
#define TRUNCATION_DETECTION_SUPPORTED 1
#else
#define TRUNCATION_DETECTION_SUPPORTED 0
#endif

namespace {

// Helper to create a UDP socket bound to a random port, returns fd and port
std::pair<int, std::uint16_t> create_test_socket() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return {-1, 0};
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // OS assigns port

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return {-1, 0};
    }

    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        close(fd);
        return {-1, 0};
    }

    return {fd, ntohs(addr.sin_port)};
}

// Helper to send data to localhost:port
bool send_to_port(std::uint16_t port, const void* data, std::size_t len) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    ssize_t sent = sendto(fd, data, len, 0,
                          reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(fd);
    return sent == static_cast<ssize_t>(len);
}

bool test_normal_reception() {
    auto [fd, port] = create_test_socket();
    if (fd < 0) {
        std::printf("Failed to create test socket\n");
        return false;
    }

    gateway::RecvConfig config{};
    config.max_datagram_bytes = 1472;
    gateway::RecvLoop recv_loop(fd, config);

    // Send a small packet
    const char* msg = "hello";
    if (!send_to_port(port, msg, 5)) {
        close(fd);
        return false;
    }

    auto result = recv_loop.recv_one();

    if (result.status != gateway::RecvStatus::Ok) {
        std::printf("Expected Ok, got %d\n", static_cast<int>(result.status));
        close(fd);
        return false;
    }

    if (result.datagram.data.size() != 5) {
        std::printf("Expected 5 bytes, got %zu\n", result.datagram.data.size());
        close(fd);
        return false;
    }

    if (recv_loop.metrics().received != 1) {
        std::printf("Expected received=1, got %llu\n", recv_loop.metrics().received);
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

bool test_tb1_truncation_detection() {
#if !TRUNCATION_DETECTION_SUPPORTED
    // Skip on platforms where MSG_TRUNC doesn't return actual packet size
    std::printf("(skipped on this platform) ");
    return true;
#else
    auto [fd, port] = create_test_socket();
    if (fd < 0) {
        std::printf("Failed to create test socket\n");
        return false;
    }

    // Set max_datagram_bytes to 100 for testing
    gateway::RecvConfig config{};
    config.max_datagram_bytes = 100;
    gateway::RecvLoop recv_loop(fd, config);

    // Send a packet larger than the limit (200 bytes)
    std::vector<char> large_packet(200, 'x');
    if (!send_to_port(port, large_packet.data(), large_packet.size())) {
        close(fd);
        return false;
    }

    auto result = recv_loop.recv_one();

    // Should detect truncation (packet > buffer size)
    if (result.status != gateway::RecvStatus::Truncated) {
        std::printf("Expected Truncated, got %d\n", static_cast<int>(result.status));
        close(fd);
        return false;
    }

    if (recv_loop.metrics().truncated != 1) {
        std::printf("Expected truncated=1, got %llu\n", recv_loop.metrics().truncated);
        close(fd);
        return false;
    }

    close(fd);
    return true;
#endif
}

bool test_tb1_exact_limit() {
    auto [fd, port] = create_test_socket();
    if (fd < 0) {
        std::printf("Failed to create test socket\n");
        return false;
    }

    // Set max_datagram_bytes to exactly 100
    gateway::RecvConfig config{};
    config.max_datagram_bytes = 100;
    gateway::RecvLoop recv_loop(fd, config);

    // Send a packet exactly at the limit
    std::vector<char> exact_packet(100, 'y');
    if (!send_to_port(port, exact_packet.data(), exact_packet.size())) {
        close(fd);
        return false;
    }

    auto result = recv_loop.recv_one();

    // Should succeed (not truncated)
    if (result.status != gateway::RecvStatus::Ok) {
        std::printf("Expected Ok for exact-limit packet, got %d\n", static_cast<int>(result.status));
        close(fd);
        return false;
    }

    if (result.datagram.data.size() != 100) {
        std::printf("Expected 100 bytes, got %zu\n", result.datagram.data.size());
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

bool test_tb1_one_over_limit() {
#if !TRUNCATION_DETECTION_SUPPORTED
    // Skip on platforms where MSG_TRUNC doesn't return actual packet size
    std::printf("(skipped on this platform) ");
    return true;
#else
    auto [fd, port] = create_test_socket();
    if (fd < 0) {
        std::printf("Failed to create test socket\n");
        return false;
    }

    // Set max_datagram_bytes to 100
    gateway::RecvConfig config{};
    config.max_datagram_bytes = 100;
    gateway::RecvLoop recv_loop(fd, config);

    // Send a packet exactly 1 byte over the limit
    std::vector<char> over_packet(101, 'z');
    if (!send_to_port(port, over_packet.data(), over_packet.size())) {
        close(fd);
        return false;
    }

    auto result = recv_loop.recv_one();

    // Should detect truncation
    if (result.status != gateway::RecvStatus::Truncated) {
        std::printf("Expected Truncated for 1-over-limit packet, got %d\n", static_cast<int>(result.status));
        close(fd);
        return false;
    }

    close(fd);
    return true;
#endif
}

bool test_source_ip_extraction() {
    auto [fd, port] = create_test_socket();
    if (fd < 0) {
        std::printf("Failed to create test socket\n");
        return false;
    }

    gateway::RecvConfig config{};
    gateway::RecvLoop recv_loop(fd, config);

    const char* msg = "test";
    if (!send_to_port(port, msg, 4)) {
        close(fd);
        return false;
    }

    auto result = recv_loop.recv_one();

    if (result.status != gateway::RecvStatus::Ok) {
        close(fd);
        return false;
    }

    // Source should be loopback (127.0.0.1 = 0x7F000001 in host byte order)
    if (result.datagram.source.ip != 0x7F000001) {
        std::printf("Expected IP 0x7F000001, got 0x%08X\n", result.datagram.source.ip);
        close(fd);
        return false;
    }

    // Port should be non-zero (ephemeral port used by sender)
    if (result.datagram.source.port == 0) {
        std::printf("Expected non-zero source port\n");
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

bool test_metrics_accumulate() {
#if !TRUNCATION_DETECTION_SUPPORTED
    // This test relies on truncation detection, skip on unsupported platforms
    std::printf("(skipped on this platform) ");
    return true;
#else
    auto [fd, port] = create_test_socket();
    if (fd < 0) {
        std::printf("Failed to create test socket\n");
        return false;
    }

    gateway::RecvConfig config{};
    config.max_datagram_bytes = 50;
    gateway::RecvLoop recv_loop(fd, config);

    // Send 2 valid packets and 2 oversized packets
    std::vector<char> small(30, 'a');
    std::vector<char> large(100, 'b');

    send_to_port(port, small.data(), small.size());  // ok
    send_to_port(port, large.data(), large.size());  // truncated
    send_to_port(port, small.data(), small.size());  // ok
    send_to_port(port, large.data(), large.size());  // truncated

    // Receive all 4
    for (int i = 0; i < 4; ++i) {
        recv_loop.recv_one();
    }

    if (recv_loop.metrics().received != 2) {
        std::printf("Expected received=2, got %llu\n", recv_loop.metrics().received);
        close(fd);
        return false;
    }

    if (recv_loop.metrics().truncated != 2) {
        std::printf("Expected truncated=2, got %llu\n", recv_loop.metrics().truncated);
        close(fd);
        return false;
    }

    close(fd);
    return true;
#endif
}

bool test_create_udp_socket() {
    // Test that create_udp_socket works
    int fd = gateway::create_udp_socket(0);  // OS-assigned port
    if (fd < 0) {
        std::printf("create_udp_socket failed\n");
        return false;
    }

    // Should be a valid socket
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        close(fd);
        return false;
    }

    // Should have been assigned a port
    if (ntohs(addr.sin_port) == 0) {
        std::printf("Expected non-zero port\n");
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

bool test_zero_byte_datagram() {
    auto [fd, port] = create_test_socket();
    if (fd < 0) {
        std::printf("Failed to create test socket\n");
        return false;
    }

    gateway::RecvConfig config{};
    gateway::RecvLoop recv_loop(fd, config);

    // Send a zero-byte datagram (valid in UDP)
    if (!send_to_port(port, "", 0)) {
        close(fd);
        return false;
    }

    auto result = recv_loop.recv_one();

    if (result.status != gateway::RecvStatus::Ok) {
        std::printf("Expected Ok for zero-byte datagram, got %d\n", static_cast<int>(result.status));
        close(fd);
        return false;
    }

    if (!result.datagram.data.empty()) {
        std::printf("Expected empty data, got %zu bytes\n", result.datagram.data.size());
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

}  // namespace

int main() {
    if (!test_normal_reception()) {
        std::printf("test_normal_reception failed\n");
        return EXIT_FAILURE;
    }

    if (!test_tb1_truncation_detection()) {
        std::printf("test_tb1_truncation_detection failed\n");
        return EXIT_FAILURE;
    }

    if (!test_tb1_exact_limit()) {
        std::printf("test_tb1_exact_limit failed\n");
        return EXIT_FAILURE;
    }

    if (!test_tb1_one_over_limit()) {
        std::printf("test_tb1_one_over_limit failed\n");
        return EXIT_FAILURE;
    }

    if (!test_source_ip_extraction()) {
        std::printf("test_source_ip_extraction failed\n");
        return EXIT_FAILURE;
    }

    if (!test_metrics_accumulate()) {
        std::printf("test_metrics_accumulate failed\n");
        return EXIT_FAILURE;
    }

    if (!test_create_udp_socket()) {
        std::printf("test_create_udp_socket failed\n");
        return EXIT_FAILURE;
    }

    if (!test_zero_byte_datagram()) {
        std::printf("test_zero_byte_datagram failed\n");
        return EXIT_FAILURE;
    }

    std::printf("All recv_loop tests passed\n");
    return EXIT_SUCCESS;
}
