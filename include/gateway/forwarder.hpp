#pragma once

#include "gateway/bounded_queue.hpp"
#include "gateway/sink.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gateway {

// ============================================================================
// TB-5: Bounded Forwarder
//
// Manages forwarding of validated events to a downstream sink with:
// 1. Bounded backlog (fixed-capacity queue)
// 2. Drop under outage (tail-drop when full)
// 3. Per-agent fairness (quota enforcement)
//
// Invariants enforced:
// - Total queue depth bounded by max_queue_depth
// - Per-agent in-flight events bounded by max_per_agent
// - Downstream slowness cannot cause unbounded backlog
// - No single agent can starve others during degradation
//
// Thread safety: NOT thread-safe. External synchronization required.
// ============================================================================

// Configuration for the forwarder
struct ForwarderConfig {
    std::size_t max_queue_depth = 4096;   // Total bounded capacity
    std::size_t max_per_agent = 64;       // Per-agent quota
};

// Result of attempting to forward an event
enum class ForwardResult : std::uint8_t {
    Queued,                    // Successfully queued for downstream
    DroppedQueueFull,          // Global queue at capacity
    DroppedAgentQuotaExceeded, // Agent using disproportionate share
};

// Event types that can be forwarded
enum class EventType : std::uint8_t {
    Metrics,
    Log,
};

// An event queued for forwarding.
// Contains copied data (not views) since original buffer may be reused.
struct QueuedEvent {
    std::string agent_id;              // Copied for quota release on dequeue
    EventType type;                    // Metrics or Log
    std::vector<std::byte> payload;    // Serialized event data
};

// ============================================================================
// AgentQuotaTracker
//
// Tracks in-flight event count per agent for fairness enforcement.
// Memory is naturally bounded by queue depth (Option C from design).
//
// Invariant: sum(all counts) == number of events in queue
// ============================================================================

class AgentQuotaTracker {
public:
    explicit AgentQuotaTracker(std::size_t max_per_agent) noexcept;

    // Attempt to reserve a slot for this agent.
    // Returns false if agent already at quota (reject event).
    [[nodiscard]] bool try_reserve(const std::string& agent_id) noexcept;

    // Release a slot when event is dequeued.
    // Must be called exactly once per successful try_reserve.
    void release(const std::string& agent_id) noexcept;

    // Current in-flight count for an agent (for testing/metrics)
    [[nodiscard]] std::size_t in_flight_count(const std::string& agent_id) const noexcept;

    // Number of distinct agents currently tracked
    [[nodiscard]] std::size_t tracked_agents() const noexcept;

    // Total events across all agents (should equal queue size)
    [[nodiscard]] std::size_t total_in_flight() const noexcept;

    // Metrics
    [[nodiscard]] std::uint64_t quota_rejections() const noexcept { return quota_rejections_; }

private:
    std::unordered_map<std::string, std::size_t> in_flight_;
    std::size_t max_per_agent_;
    std::size_t total_in_flight_ = 0;
    std::uint64_t quota_rejections_ = 0;
};

// ============================================================================
// BoundedForwarder
//
// Main forwarder component combining queue + quota tracker + sink.
// ============================================================================

class BoundedForwarder {
public:
    // Construct with config and sink.
    // Takes ownership of the sink.
    explicit BoundedForwarder(ForwarderConfig config, std::unique_ptr<Sink> sink);

    // Attempt to forward an event.
    // Non-blocking: returns immediately with result.
    //
    // Order of checks:
    // 1. Agent quota (fairness)
    // 2. Queue capacity (backlog bound)
    [[nodiscard]] ForwardResult try_forward(QueuedEvent event) noexcept;

    // Process one event from the queue.
    // Pops from queue, releases agent quota, writes to sink.
    // Returns true if an event was processed, false if queue was empty.
    //
    // Call this from a drain loop or worker thread.
    bool drain_one() noexcept;

    // Process all available events in the queue.
    // Returns number of events processed.
    std::size_t drain_all() noexcept;

    // Current queue depth
    [[nodiscard]] std::size_t queue_depth() const noexcept;

    // Queue capacity
    [[nodiscard]] std::size_t queue_capacity() const noexcept;

    // Check if queue is empty
    [[nodiscard]] bool queue_empty() const noexcept;

    // Access quota tracker (for metrics/testing)
    [[nodiscard]] const AgentQuotaTracker& quota_tracker() const noexcept;

    // Metrics
    [[nodiscard]] std::uint64_t total_forwarded() const noexcept { return total_forwarded_; }
    [[nodiscard]] std::uint64_t total_dropped_queue_full() const noexcept { return dropped_queue_full_; }
    [[nodiscard]] std::uint64_t total_dropped_quota() const noexcept { return dropped_quota_; }
    [[nodiscard]] std::uint64_t total_sink_failures() const noexcept { return sink_failures_; }

private:
    ForwarderConfig config_;
    AgentQuotaTracker quota_tracker_;
    BoundedQueue<QueuedEvent> queue_;
    std::unique_ptr<Sink> sink_;

    // Metrics
    std::uint64_t total_forwarded_ = 0;
    std::uint64_t dropped_queue_full_ = 0;
    std::uint64_t dropped_quota_ = 0;
    std::uint64_t sink_failures_ = 0;
};

}  // namespace gateway
