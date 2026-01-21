// Traffic Generator Demo
//
// Simulates multiple agents sending telemetry data to the gateway.
//
// Usage:
//   ./traffic_generator [host] [port] [--chaos]
//
// Options:
//   host   - Target host (default: 127.0.0.1)
//   port   - Target port (default: 9999)
//   --chaos - Enable chaos mode (sends malformed data, bursts, etc.)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int /*signum*/) {
    g_running = false;
}

// Simple random number generator
class Random {
public:
    Random() : gen_(std::random_device{}()) {}

    int range(int min, int max) {
        std::uniform_int_distribution<int> dist(min, max);
        return dist(gen_);
    }

    double uniform() {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return dist(gen_);
    }

    template<typename T>
    const T& pick(const std::vector<T>& vec) {
        return vec[range(0, static_cast<int>(vec.size()) - 1)];
    }

private:
    std::mt19937 gen_;
};

// Agent identifiers
const std::vector<std::string> AGENTS = {
    "webserver01", "webserver02", "webserver03",
    "dbmaster", "dbreplica01", "dbreplica02",
    "cachenode01", "cachenode02",
    "worker01", "worker02", "worker03", "worker04",
    "gateway", "loadbalancer", "scheduler"
};

// Metric names
const std::vector<std::string> METRIC_NAMES = {
    "cpu_percent", "memory_used_bytes", "disk_io_bytes",
    "network_rx_bytes", "network_tx_bytes",
    "request_count", "request_latency_ms", "error_count",
    "queue_depth", "active_connections", "cache_hit_ratio",
    "gc_pause_ms", "thread_count", "heap_used_bytes"
};

// Log levels and messages
const std::vector<std::string> LOG_LEVELS = {
    "trace", "debug", "info", "warn", "error"
};

const std::vector<std::string> LOG_MESSAGES = {
    "Request processed successfully",
    "Connection established",
    "Cache miss for key",
    "Retry attempt",
    "Configuration reloaded",
    "Health check passed",
    "Database query completed",
    "Background job started",
    "Rate limit exceeded",
    "Authentication successful",
    "Session expired",
    "Timeout waiting for response",
    "Invalid input received",
    "Resource not found",
    "Permission denied"
};

// Get current timestamp in milliseconds
std::uint64_t now_ms() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

// Create envelope: 2-byte length prefix (big-endian) + body
std::vector<std::byte> make_envelope(const std::string& body) {
    std::uint16_t len = static_cast<std::uint16_t>(body.size());
    std::vector<std::byte> result(2 + body.size());

    // Big-endian length prefix (network byte order)
    result[0] = static_cast<std::byte>((len >> 8) & 0xFF);  // high byte first
    result[1] = static_cast<std::byte>(len & 0xFF);         // low byte second

    std::memcpy(result.data() + 2, body.data(), body.size());
    return result;
}

// Generate a metrics message
std::string make_metrics_json(Random& rng, const std::string& agent_id, int seq) {
    std::string json = "{";
    json += "\"agent_id\":\"" + agent_id + "\",";
    json += "\"seq\":" + std::to_string(seq) + ",";
    json += "\"ts\":" + std::to_string(now_ms()) + ",";
    json += "\"metrics\":[";

    int metric_count = rng.range(1, 5);
    for (int i = 0; i < metric_count; ++i) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"n\":\"" + rng.pick(METRIC_NAMES) + "\",";
        json += "\"v\":" + std::to_string(rng.uniform() * 1000);

        // Sometimes add unit
        if (rng.uniform() > 0.5) {
            json += ",\"u\":\"bytes\"";
        }

        // Sometimes add tags
        if (rng.uniform() > 0.7) {
            json += ",\"tags\":{\"env\":\"prod\",\"region\":\"us-east\"}";
        }

        json += "}";
    }

    json += "]}";
    return json;
}

// Generate a log message in logfmt format
// Format: ts=<timestamp> level=<level> agent=<agent_id> msg="<message>" [extra fields...]
std::string make_log_logfmt(Random& rng, const std::string& agent_id) {
    std::string level = rng.pick(LOG_LEVELS);
    std::string msg = rng.pick(LOG_MESSAGES);

    std::string logfmt;
    logfmt += "ts=" + std::to_string(now_ms());
    logfmt += " level=" + level;
    logfmt += " agent=" + agent_id;
    logfmt += " msg=\"" + msg + "\"";

    // Sometimes add extra fields
    if (rng.uniform() > 0.5) {
        logfmt += " request_id=req-" + std::to_string(rng.range(1000, 9999));
        if (rng.uniform() > 0.5) {
            logfmt += " duration_ms=" + std::to_string(rng.range(1, 500));
        }
    }

    return logfmt;
}

// ============================================================================
// Chaos mode: Generate problematic traffic
// ============================================================================

// Oversized datagram (> 1472 bytes)
std::vector<std::byte> make_oversized() {
    std::string huge(2000, 'X');
    return make_envelope(huge);
}

// Malformed envelope (wrong length)
std::vector<std::byte> make_bad_envelope() {
    std::string body = "{\"agent_id\":\"test\"}";
    auto envelope = make_envelope(body);
    // Corrupt the length field
    envelope[0] = std::byte{0xFF};
    envelope[1] = std::byte{0xFF};
    return envelope;
}

// Invalid JSON
std::vector<std::byte> make_bad_json() {
    return make_envelope("{\"agent_id\": broken json here");
}

// Old timestamp (should fail validation)
std::string make_old_timestamp_metrics(const std::string& agent_id, int seq) {
    std::uint64_t old_ts = now_ms() - 3600000;  // 1 hour ago
    std::string json = "{";
    json += "\"agent_id\":\"" + agent_id + "\",";
    json += "\"seq\":" + std::to_string(seq) + ",";
    json += "\"ts\":" + std::to_string(old_ts) + ",";
    json += "\"metrics\":[{\"n\":\"cpu\",\"v\":50}]}";
    return json;
}

// Invalid agent ID
std::string make_bad_agent_metrics(int seq) {
    std::string json = "{";
    json += "\"agent_id\":\"123-invalid-starts-with-number\",";
    json += "\"seq\":" + std::to_string(seq) + ",";
    json += "\"ts\":" + std::to_string(now_ms()) + ",";
    json += "\"metrics\":[{\"n\":\"cpu\",\"v\":50}]}";
    return json;
}

// Statistics
struct Stats {
    std::uint64_t metrics_sent = 0;
    std::uint64_t logs_sent = 0;
    std::uint64_t chaos_sent = 0;
    std::uint64_t send_errors = 0;
};

void print_stats(const Stats& stats) {
    std::fprintf(stderr, "--- Generator Stats ---\n");
    std::fprintf(stderr, "Metrics sent: %lu\n", stats.metrics_sent);
    std::fprintf(stderr, "Logs sent:    %lu\n", stats.logs_sent);
    std::fprintf(stderr, "Chaos sent:   %lu\n", stats.chaos_sent);
    std::fprintf(stderr, "Send errors:  %lu\n", stats.send_errors);
    std::fprintf(stderr, "-----------------------\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    // Parse arguments
    const char* host = "127.0.0.1";
    std::uint16_t port = 9999;
    bool chaos_mode = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--chaos") == 0) {
            chaos_mode = true;
        } else if (i == 1 && argv[i][0] != '-') {
            host = argv[i];
        } else if (i == 2 && argv[i][0] != '-') {
            port = static_cast<std::uint16_t>(std::atoi(argv[i]));
        }
    }

    std::fprintf(stderr, "Traffic generator targeting %s:%u%s\n",
                 host, port, chaos_mode ? " (chaos mode)" : "");

    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create UDP socket
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "Failed to create socket\n");
        return EXIT_FAILURE;
    }

    // Set up destination address
    struct sockaddr_in dest_addr{};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dest_addr.sin_addr) <= 0) {
        std::fprintf(stderr, "Invalid address: %s\n", host);
        close(fd);
        return EXIT_FAILURE;
    }

    Random rng;
    Stats stats;
    std::vector<int> agent_seqs(AGENTS.size(), 0);

    auto last_stats_time = std::chrono::steady_clock::now();
    int burst_counter = 0;

    std::fprintf(stderr, "Generating traffic. Press Ctrl+C to stop.\n\n");

    while (g_running) {
        std::vector<std::byte> packet;
        bool is_chaos = false;

        // In chaos mode, occasionally send problematic traffic
        if (chaos_mode && rng.uniform() < 0.1) {
            is_chaos = true;
            int chaos_type = rng.range(0, 4);

            switch (chaos_type) {
                case 0:
                    // Oversized
                    packet = make_oversized();
                    break;
                case 1:
                    // Bad envelope
                    packet = make_bad_envelope();
                    break;
                case 2:
                    // Bad JSON
                    packet = make_bad_json();
                    break;
                case 3:
                    // Old timestamp
                    packet = make_envelope(make_old_timestamp_metrics(
                        rng.pick(AGENTS), rng.range(0, 1000)));
                    break;
                case 4:
                    // Bad agent ID
                    packet = make_envelope(make_bad_agent_metrics(rng.range(0, 1000)));
                    break;
            }
        } else {
            // Normal traffic
            int agent_idx = rng.range(0, static_cast<int>(AGENTS.size()) - 1);
            const std::string& agent = AGENTS[agent_idx];
            int& seq = agent_seqs[agent_idx];

            if (rng.uniform() < 0.7) {
                // 70% metrics
                packet = make_envelope(make_metrics_json(rng, agent, seq++));
                ++stats.metrics_sent;
            } else {
                // 30% logs
                packet = make_envelope(make_log_logfmt(rng, agent));
                ++stats.logs_sent;
            }
        }

        // Send packet
        ssize_t sent = sendto(fd, packet.data(), packet.size(), 0,
                              reinterpret_cast<struct sockaddr*>(&dest_addr),
                              sizeof(dest_addr));

        if (sent < 0) {
            ++stats.send_errors;
        } else if (is_chaos) {
            ++stats.chaos_sent;
        }

        // In chaos mode, occasionally burst from one agent (to trigger quota limits)
        if (chaos_mode && rng.uniform() < 0.05) {
            const std::string& burst_agent = rng.pick(AGENTS);
            std::fprintf(stderr, "[CHAOS] Bursting 50 packets from %s\n", burst_agent.c_str());

            for (int i = 0; i < 50 && g_running; ++i) {
                auto burst_packet = make_envelope(make_metrics_json(rng, burst_agent, i));
                sendto(fd, burst_packet.data(), burst_packet.size(), 0,
                       reinterpret_cast<struct sockaddr*>(&dest_addr),
                       sizeof(dest_addr));
                ++stats.chaos_sent;
            }
        }

        // Print stats every second
        auto now = std::chrono::steady_clock::now();
        if (now - last_stats_time >= std::chrono::seconds(1)) {
            print_stats(stats);
            last_stats_time = now;
        }

        // Rate limiting: ~100 packets/sec normally, faster in chaos mode
        int delay_ms = chaos_mode ? rng.range(1, 10) : rng.range(5, 15);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    std::fprintf(stderr, "\nShutting down...\n");
    print_stats(stats);

    close(fd);
    return EXIT_SUCCESS;
}
