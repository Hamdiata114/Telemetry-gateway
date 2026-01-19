#include "gateway/recv_loop.hpp"

#include <cerrno>
#include <cstring>  // memset

// Platform headers
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gateway {

RecvLoop::RecvLoop(int fd, RecvConfig config)
    : fd_(fd)
    , config_(config)
    , buffer_(config.max_datagram_bytes) {}

bool RecvLoop::configure_socket() {
    // Set receive buffer size
    int bufsize = static_cast<int>(config_.recv_buffer_bytes);
    if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) < 0) {
        return false;
    }

    // Reject IP fragmentation (Linux-specific)
    // This tells the kernel to drop fragmented packets
#ifdef IP_PMTUDISC_DO
    int pmtu = IP_PMTUDISC_DO;
    if (setsockopt(fd_, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof(pmtu)) < 0) {
        // Non-fatal on platforms that don't support this
    }
#endif

    return true;
}

RecvResult RecvLoop::recv_one() {
    RecvResult result{};
    result.status = RecvStatus::Error;

    sockaddr_in src_addr{};
    socklen_t addr_len = sizeof(src_addr);

    // MSG_TRUNC (Linux): returns real packet size even if truncated
    // This lets us detect oversized packets
    ssize_t n = recvfrom(
        fd_,
        buffer_.data(),
        buffer_.size(),
        MSG_TRUNC,
        reinterpret_cast<sockaddr*>(&src_addr),
        &addr_len
    );

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            result.status = RecvStatus::WouldBlock;
        } else {
            result.status = RecvStatus::Error;
            result.error_code = errno;
            ++metrics_.errors;
        }
        return result;
    }

    // Check for truncation (packet was larger than buffer)
    if (static_cast<std::size_t>(n) > buffer_.size()) {
        result.status = RecvStatus::Truncated;
        ++metrics_.truncated;
        return result;
    }

    // Success: copy data and extract source
    result.status = RecvStatus::Ok;
    result.datagram.data.assign(buffer_.begin(), buffer_.begin() + n);
    result.datagram.source.ip = ntohl(src_addr.sin_addr.s_addr);
    result.datagram.source.port = ntohs(src_addr.sin_port);

    ++metrics_.received;
    return result;
}

int create_udp_socket(std::uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

}  // namespace gateway
