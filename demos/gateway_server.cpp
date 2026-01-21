// Gateway Server Demo
//
// Full end-to-end pipeline: UDP recv → TB-1 → TB-5 → Sink
//
// Usage:
//   ./gateway_server [port] [--slow]
//
// Options:
//   port   - UDP port to listen on (default: 9999)
//   --slow - Enable slow sink mode (100ms delay per write)

#include "gateway/config.hpp"
#include "gateway/forwarder.hpp"
#include "gateway/parse_envelope.hpp"
#include "gateway/parse_log.hpp"
#include "gateway/parse_metrics.hpp"
#include "gateway/recv_loop.hpp"
#include "gateway/sink.hpp"
#include "gateway/source_limiter.hpp"
#include "gateway/validate_log.hpp"
#include "gateway/validate_metrics.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>  // close()

namespace {

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

void signal_handler(int /*signum*/) {
    g_running = false;
}

// Statistics
struct Stats {
    std::uint64_t received = 0;
    std::uint64_t source_limited = 0;
    std::uint64_t envelope_drops = 0;
    std::uint64_t parse_drops = 0;
    std::uint64_t validation_drops = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t queue_drops = 0;
    std::uint64_t quota_drops = 0;
};

// Detect message type by looking at format
// - Metrics: JSON with "metrics" array
// - Log: logfmt with key=value pairs
enum class MessageType { Metrics, Log, Unknown };

MessageType detect_message_type(std::span<const std::byte> body) {
    if (body.empty()) return MessageType::Unknown;

    std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());

    // JSON starts with '{', logfmt starts with a key like 'ts='
    if (!text.empty() && text[0] == '{') {
        // It's JSON - check for metrics array
        if (text.find("\"metrics\"") != std::string_view::npos) {
            return MessageType::Metrics;
        }
    } else {
        // Not JSON - assume logfmt if it has required log fields
        if (text.find("ts=") != std::string_view::npos &&
            text.find("level=") != std::string_view::npos &&
            text.find("msg=") != std::string_view::npos) {
            return MessageType::Log;
        }
    }
    return MessageType::Unknown;
}

// Get current time in milliseconds (for validation)
std::uint64_t current_time_ms() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch());
    return static_cast<std::uint64_t>(ms.count());
}

// Convert validated event to JSON bytes for forwarding
std::vector<std::byte> serialize_event(const gateway::ValidatedMetrics& metrics) {
    // For demo, just re-serialize a simplified JSON
    std::string json = "{\"type\":\"metrics\",\"agent_id\":\"";
    json += metrics.agent_id;
    json += "\",\"seq\":";
    json += std::to_string(metrics.seq);
    json += ",\"ts\":";
    json += std::to_string(metrics.ts);
    json += ",\"metric_count\":";
    json += std::to_string(metrics.metric_count);
    json += "}";

    std::vector<std::byte> result(json.size());
    std::memcpy(result.data(), json.data(), json.size());
    return result;
}

std::vector<std::byte> serialize_event(const gateway::ValidatedLog& log) {
    std::string json = "{\"type\":\"log\",\"agent_id\":\"";
    json += log.agent_id;
    json += "\",\"ts\":";
    json += std::to_string(log.ts);
    json += ",\"level\":";
    json += std::to_string(static_cast<int>(log.level));
    json += ",\"msg\":\"";
    // Escape any quotes in message (simplified)
    for (char c : log.msg) {
        if (c == '"') json += "\\\"";
        else if (c == '\\') json += "\\\\";
        else json += c;
    }
    json += "\"}";

    std::vector<std::byte> result(json.size());
    std::memcpy(result.data(), json.data(), json.size());
    return result;
}

void print_stats(const Stats& stats, const gateway::BoundedForwarder& forwarder,
                 const gateway::SourceLimiter& limiter) {
    std::fprintf(stderr, "\n--- Stats ---\n");
    std::fprintf(stderr, "Received:        %lu\n", stats.received);
    std::fprintf(stderr, "Source limited:  %lu\n", stats.source_limited);
    std::fprintf(stderr, "Envelope drops:  %lu\n", stats.envelope_drops);
    std::fprintf(stderr, "Parse drops:     %lu\n", stats.parse_drops);
    std::fprintf(stderr, "Validation drops:%lu\n", stats.validation_drops);
    std::fprintf(stderr, "Queue drops:     %lu (queue full)\n", stats.queue_drops);
    std::fprintf(stderr, "Quota drops:     %lu (per-agent)\n", stats.quota_drops);
    std::fprintf(stderr, "Forwarded:       %lu\n", forwarder.total_forwarded());
    std::fprintf(stderr, "Queue depth:     %zu / %zu\n",
                 forwarder.queue_depth(), forwarder.queue_capacity());
    std::fprintf(stderr, "Tracked agents:  %zu\n",
                 forwarder.quota_tracker().tracked_agents());
    std::fprintf(stderr, "Source limiter:  %zu sources tracked\n",
                 limiter.tracked_count());
    std::fprintf(stderr, "-------------\n\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    // Parse arguments
    std::uint16_t port = 9999;
    bool slow_mode = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--slow") == 0) {
            slow_mode = true;
        } else {
            port = static_cast<std::uint16_t>(std::atoi(argv[i]));
        }
    }

    std::fprintf(stderr, "Starting gateway server on port %u%s\n",
                 port, slow_mode ? " (slow mode)" : "");

    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create UDP socket
    int fd = gateway::create_udp_socket(port);
    if (fd < 0) {
        std::fprintf(stderr, "Failed to create UDP socket on port %u\n", port);
        return EXIT_FAILURE;
    }

    // Initialize pipeline components
    gateway::RecvConfig recv_config;
    gateway::RecvLoop recv_loop(fd, recv_config);
    if (!recv_loop.configure_socket()) {
        std::fprintf(stderr, "Failed to configure socket\n");
        return EXIT_FAILURE;
    }

    gateway::SourceLimiterConfig limiter_config;
    limiter_config.tokens_per_sec = 50;   // 50 packets/sec sustained
    limiter_config.burst_tokens = 100;    // Allow bursts up to 100
    gateway::SourceLimiter source_limiter(limiter_config);

    gateway::ForwarderConfig forwarder_config;
    forwarder_config.max_queue_depth = 256;  // Small for demo visibility
    forwarder_config.max_per_agent = 16;     // Per-agent quota

    std::unique_ptr<gateway::Sink> sink;
    if (slow_mode) {
        sink = std::make_unique<gateway::SlowSink>(
            std::make_unique<gateway::StdoutJsonSink>(),
            100  // 100ms delay
        );
    } else {
        sink = std::make_unique<gateway::StdoutJsonSink>();
    }

    gateway::BoundedForwarder forwarder(forwarder_config, std::move(sink));

    // Validation configs
    gateway::MetricsValidationConfig metrics_validation;
    gateway::LogValidationConfig log_validation;

    Stats stats;
    auto last_stats_time = std::chrono::steady_clock::now();

    std::fprintf(stderr, "Gateway ready. Press Ctrl+C to stop.\n");
    std::fprintf(stderr, "Forwarded events will be printed as JSON to stdout.\n\n");

    // Main loop
    while (g_running) {
        // Receive datagram
        auto result = recv_loop.recv_one();

        if (result.status == gateway::RecvStatus::WouldBlock) {
            // No data, drain forwarder and check stats
            forwarder.drain_one();

            // Print stats every second
            auto now = std::chrono::steady_clock::now();
            if (now - last_stats_time >= std::chrono::seconds(1)) {
                print_stats(stats, forwarder, source_limiter);
                last_stats_time = now;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (result.status == gateway::RecvStatus::Error) {
            if (g_running) {
                std::fprintf(stderr, "Recv error: %d\n", result.error_code);
            }
            continue;
        }

        if (result.status == gateway::RecvStatus::Truncated) {
            // TB-1: Oversized datagram dropped
            continue;
        }

        ++stats.received;

        // TB-1.5: Source rate limiting
        if (source_limiter.admit(result.datagram.source) == gateway::Admit::Drop) {
            ++stats.source_limited;
            continue;
        }

        // TB-2: Envelope parsing
        auto envelope_result = gateway::parse_envelope(
            std::span<const std::byte>(result.datagram.data));

        if (std::holds_alternative<gateway::DropReason>(envelope_result)) {
            ++stats.envelope_drops;
            continue;
        }

        auto& parsed_body = std::get<gateway::ParsedBody>(envelope_result);

        // Detect message type
        auto msg_type = detect_message_type(parsed_body.body);

        std::uint64_t now_ms = current_time_ms();

        if (msg_type == MessageType::Metrics) {
            // TB-3: Parse metrics
            auto parse_result = gateway::parse_metrics(parsed_body.body);
            if (std::holds_alternative<gateway::MetricsDropReason>(parse_result)) {
                ++stats.parse_drops;
                continue;
            }

            auto& parsed = std::get<gateway::ParsedMetrics>(parse_result);

            // TB-4: Validate metrics
            auto validate_result = gateway::validate_metrics(
                parsed, metrics_validation, now_ms);
            if (std::holds_alternative<gateway::MetricsValidationDrop>(validate_result)) {
                ++stats.validation_drops;
                continue;
            }

            auto& validated = std::get<gateway::ValidatedMetrics>(validate_result);

            // TB-5: Forward
            gateway::QueuedEvent event;
            event.agent_id = std::string(validated.agent_id);
            event.type = gateway::EventType::Metrics;
            event.payload = serialize_event(validated);

            auto forward_result = forwarder.try_forward(std::move(event));
            if (forward_result == gateway::ForwardResult::DroppedQueueFull) {
                ++stats.queue_drops;
            } else if (forward_result == gateway::ForwardResult::DroppedAgentQuotaExceeded) {
                ++stats.quota_drops;
            }

        } else if (msg_type == MessageType::Log) {
            // TB-3: Parse log
            auto parse_result = gateway::parse_log(parsed_body.body);
            if (std::holds_alternative<gateway::LogDropReason>(parse_result)) {
                ++stats.parse_drops;
                continue;
            }

            auto& parsed = std::get<gateway::ParsedLog>(parse_result);

            // TB-4: Validate log
            auto validate_result = gateway::validate_log(
                parsed, log_validation, now_ms);
            if (std::holds_alternative<gateway::LogValidationDrop>(validate_result)) {
                ++stats.validation_drops;
                continue;
            }

            auto& validated = std::get<gateway::ValidatedLog>(validate_result);

            // TB-5: Forward
            gateway::QueuedEvent event;
            event.agent_id = std::string(validated.agent_id);
            event.type = gateway::EventType::Log;
            event.payload = serialize_event(validated);

            auto forward_result = forwarder.try_forward(std::move(event));
            if (forward_result == gateway::ForwardResult::DroppedQueueFull) {
                ++stats.queue_drops;
            } else if (forward_result == gateway::ForwardResult::DroppedAgentQuotaExceeded) {
                ++stats.quota_drops;
            }

        } else {
            // Unknown message type
            ++stats.parse_drops;
            continue;
        }

        // Drain forwarder
        forwarder.drain_one();

        // Print stats every second
        auto now = std::chrono::steady_clock::now();
        if (now - last_stats_time >= std::chrono::seconds(1)) {
            print_stats(stats, forwarder, source_limiter);
            last_stats_time = now;
        }
    }

    // Final drain
    std::fprintf(stderr, "\nShutting down, draining queue...\n");
    forwarder.drain_all();

    // Final stats
    print_stats(stats, forwarder, source_limiter);

    close(fd);
    std::fprintf(stderr, "Goodbye.\n");
    return EXIT_SUCCESS;
}
