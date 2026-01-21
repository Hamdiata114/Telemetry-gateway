#include "gateway/forwarder.hpp"
#include "gateway/sink.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Helper to create a test event
gateway::QueuedEvent make_event(const std::string& agent_id,
                                 gateway::EventType type = gateway::EventType::Metrics) {
    gateway::QueuedEvent event;
    event.agent_id = agent_id;
    event.type = type;
    event.payload = {std::byte{0x01}, std::byte{0x02}};  // Dummy payload
    return event;
}

// ============================================================================
// AgentQuotaTracker Tests
// ============================================================================

bool test_quota_basic_reserve_release() {
    gateway::AgentQuotaTracker tracker(3);  // max 3 per agent

    // Reserve slots for agent A
    if (!tracker.try_reserve("agentA")) return false;
    if (tracker.in_flight_count("agentA") != 1) return false;

    if (!tracker.try_reserve("agentA")) return false;
    if (tracker.in_flight_count("agentA") != 2) return false;

    // Release one
    tracker.release("agentA");
    if (tracker.in_flight_count("agentA") != 1) return false;

    // Release another
    tracker.release("agentA");
    if (tracker.in_flight_count("agentA") != 0) return false;

    // Agent should be pruned from tracking
    if (tracker.tracked_agents() != 0) return false;

    return true;
}

bool test_quota_enforcement() {
    gateway::AgentQuotaTracker tracker(2);  // max 2 per agent

    // Fill quota
    if (!tracker.try_reserve("agentA")) return false;
    if (!tracker.try_reserve("agentA")) return false;

    // Should reject - quota exceeded
    if (tracker.try_reserve("agentA")) return false;
    if (tracker.quota_rejections() != 1) return false;

    // Another agent should still be able to reserve
    if (!tracker.try_reserve("agentB")) return false;
    if (tracker.tracked_agents() != 2) return false;

    return true;
}

bool test_quota_multiple_agents() {
    gateway::AgentQuotaTracker tracker(2);

    // Two agents, each at quota
    tracker.try_reserve("A");
    tracker.try_reserve("A");
    tracker.try_reserve("B");
    tracker.try_reserve("B");

    if (tracker.total_in_flight() != 4) return false;
    if (tracker.tracked_agents() != 2) return false;

    // Both should be rejected
    if (tracker.try_reserve("A")) return false;
    if (tracker.try_reserve("B")) return false;

    // Release one from A
    tracker.release("A");
    if (tracker.in_flight_count("A") != 1) return false;

    // Now A can reserve again
    if (!tracker.try_reserve("A")) return false;

    return true;
}

bool test_quota_release_unknown_agent() {
    gateway::AgentQuotaTracker tracker(2);

    // Releasing unknown agent should be safe (no-op)
    tracker.release("unknown");
    if (tracker.tracked_agents() != 0) return false;

    return true;
}

// ============================================================================
// BoundedForwarder Tests - Invariant 1: Bounded Backlog
// ============================================================================

bool test_forwarder_bounded_backlog() {
    gateway::ForwarderConfig config;
    config.max_queue_depth = 3;
    config.max_per_agent = 10;  // High quota so queue fills first

    auto sink = std::make_unique<gateway::NullSink>();
    gateway::BoundedForwarder forwarder(config, std::move(sink));

    // Fill the queue (3 different agents to avoid quota limits)
    if (forwarder.try_forward(make_event("A")) != gateway::ForwardResult::Queued) return false;
    if (forwarder.try_forward(make_event("B")) != gateway::ForwardResult::Queued) return false;
    if (forwarder.try_forward(make_event("C")) != gateway::ForwardResult::Queued) return false;

    if (forwarder.queue_depth() != 3) return false;

    // Queue is full - next should be dropped
    if (forwarder.try_forward(make_event("D")) != gateway::ForwardResult::DroppedQueueFull) return false;
    if (forwarder.total_dropped_queue_full() != 1) return false;

    // Queue depth unchanged
    if (forwarder.queue_depth() != 3) return false;

    return true;
}

bool test_forwarder_queue_drains() {
    gateway::ForwarderConfig config;
    config.max_queue_depth = 2;
    config.max_per_agent = 10;

    auto sink = std::make_unique<gateway::NullSink>();
    auto* sink_ptr = sink.get();  // Keep pointer for checking
    gateway::BoundedForwarder forwarder(config, std::move(sink));

    // Queue two events
    forwarder.try_forward(make_event("A"));
    forwarder.try_forward(make_event("B"));

    // Drain one
    if (!forwarder.drain_one()) return false;
    if (forwarder.queue_depth() != 1) return false;
    if (sink_ptr->write_count() != 1) return false;

    // Now we can queue another
    if (forwarder.try_forward(make_event("C")) != gateway::ForwardResult::Queued) return false;

    // Drain all
    if (forwarder.drain_all() != 2) return false;
    if (!forwarder.queue_empty()) return false;
    if (sink_ptr->write_count() != 3) return false;

    return true;
}

// ============================================================================
// BoundedForwarder Tests - Invariant 2: Drop Under Outage
// ============================================================================

bool test_forwarder_drop_under_backpressure() {
    gateway::ForwarderConfig config;
    config.max_queue_depth = 2;
    config.max_per_agent = 100;  // High quota

    auto sink = std::make_unique<gateway::NullSink>();
    gateway::BoundedForwarder forwarder(config, std::move(sink));

    // Simulate downstream outage by not draining
    forwarder.try_forward(make_event("A"));
    forwarder.try_forward(make_event("B"));

    // Queue full, downstream "slow" - should drop, not block
    auto result = forwarder.try_forward(make_event("C"));
    if (result != gateway::ForwardResult::DroppedQueueFull) return false;

    // Drop many more
    for (int i = 0; i < 100; ++i) {
        forwarder.try_forward(make_event("X"));
    }

    // Queue depth still bounded
    if (forwarder.queue_depth() != 2) return false;
    if (forwarder.total_dropped_queue_full() != 101) return false;

    return true;
}

bool test_forwarder_sink_failure_handling() {
    gateway::ForwarderConfig config;
    config.max_queue_depth = 2;
    config.max_per_agent = 10;

    auto sink = std::make_unique<gateway::FailingSink>();
    gateway::BoundedForwarder forwarder(config, std::move(sink));

    forwarder.try_forward(make_event("A"));
    forwarder.try_forward(make_event("B"));

    // Drain should still work (quota released) even if sink fails
    if (!forwarder.drain_one()) return false;
    if (forwarder.total_sink_failures() != 1) return false;
    if (forwarder.queue_depth() != 1) return false;

    // Quota should be released even on sink failure
    // Agent A should be able to queue again
    if (forwarder.try_forward(make_event("A")) != gateway::ForwardResult::Queued) return false;

    return true;
}

// ============================================================================
// BoundedForwarder Tests - Invariant 3: Per-Agent Fairness
// ============================================================================

bool test_forwarder_per_agent_quota() {
    gateway::ForwarderConfig config;
    config.max_queue_depth = 100;  // Large queue
    config.max_per_agent = 2;      // Small per-agent quota

    auto sink = std::make_unique<gateway::NullSink>();
    gateway::BoundedForwarder forwarder(config, std::move(sink));

    // Agent A fills its quota
    if (forwarder.try_forward(make_event("A")) != gateway::ForwardResult::Queued) return false;
    if (forwarder.try_forward(make_event("A")) != gateway::ForwardResult::Queued) return false;

    // Agent A exceeds quota - rejected
    if (forwarder.try_forward(make_event("A")) != gateway::ForwardResult::DroppedAgentQuotaExceeded) return false;
    if (forwarder.total_dropped_quota() != 1) return false;

    // But agent B can still queue (fairness!)
    if (forwarder.try_forward(make_event("B")) != gateway::ForwardResult::Queued) return false;
    if (forwarder.try_forward(make_event("B")) != gateway::ForwardResult::Queued) return false;

    // B also hits quota
    if (forwarder.try_forward(make_event("B")) != gateway::ForwardResult::DroppedAgentQuotaExceeded) return false;

    return true;
}

bool test_forwarder_fairness_under_pressure() {
    gateway::ForwarderConfig config;
    config.max_queue_depth = 10;
    config.max_per_agent = 2;

    auto sink = std::make_unique<gateway::NullSink>();
    gateway::BoundedForwarder forwarder(config, std::move(sink));

    // 5 agents each try to queue 3 events (exceeds their quota of 2)
    int queued_count = 0;
    int quota_drops = 0;

    for (int agent = 0; agent < 5; ++agent) {
        std::string agent_id = "agent" + std::to_string(agent);
        for (int i = 0; i < 3; ++i) {
            auto result = forwarder.try_forward(make_event(agent_id));
            if (result == gateway::ForwardResult::Queued) {
                ++queued_count;
            } else if (result == gateway::ForwardResult::DroppedAgentQuotaExceeded) {
                ++quota_drops;
            }
        }
    }

    // Each agent should have exactly 2 in queue (quota), 1 dropped
    if (queued_count != 10) return false;  // 5 agents * 2 each
    if (quota_drops != 5) return false;     // 5 agents * 1 excess each
    if (forwarder.queue_depth() != 10) return false;

    return true;
}

bool test_forwarder_quota_releases_on_drain() {
    gateway::ForwarderConfig config;
    config.max_queue_depth = 10;
    config.max_per_agent = 2;

    auto sink = std::make_unique<gateway::NullSink>();
    gateway::BoundedForwarder forwarder(config, std::move(sink));

    // Agent A fills quota
    forwarder.try_forward(make_event("A"));
    forwarder.try_forward(make_event("A"));

    // Rejected
    if (forwarder.try_forward(make_event("A")) != gateway::ForwardResult::DroppedAgentQuotaExceeded) return false;

    // Drain one event from A
    forwarder.drain_one();

    // Now A can queue again
    if (forwarder.try_forward(make_event("A")) != gateway::ForwardResult::Queued) return false;

    return true;
}

bool test_forwarder_quota_tracker_bounded_by_queue() {
    gateway::ForwarderConfig config;
    config.max_queue_depth = 4;
    config.max_per_agent = 2;

    auto sink = std::make_unique<gateway::NullSink>();
    gateway::BoundedForwarder forwarder(config, std::move(sink));

    // Queue events from many agents
    forwarder.try_forward(make_event("A"));
    forwarder.try_forward(make_event("B"));
    forwarder.try_forward(make_event("C"));
    forwarder.try_forward(make_event("D"));

    // Tracker should have 4 agents
    if (forwarder.quota_tracker().tracked_agents() != 4) return false;
    if (forwarder.quota_tracker().total_in_flight() != 4) return false;

    // Drain all
    forwarder.drain_all();

    // Tracker should be empty (all agents pruned)
    if (forwarder.quota_tracker().tracked_agents() != 0) return false;
    if (forwarder.quota_tracker().total_in_flight() != 0) return false;

    return true;
}

// ============================================================================
// Edge Cases
// ============================================================================

bool test_forwarder_empty_queue_drain() {
    gateway::ForwarderConfig config;
    auto sink = std::make_unique<gateway::NullSink>();
    gateway::BoundedForwarder forwarder(config, std::move(sink));

    // Draining empty queue should return false
    if (forwarder.drain_one()) return false;
    if (forwarder.drain_all() != 0) return false;

    return true;
}

bool test_forwarder_queue_full_releases_quota() {
    // Regression test: when queue is full, the quota reservation must be released
    gateway::ForwarderConfig config;
    config.max_queue_depth = 1;
    config.max_per_agent = 10;

    auto sink = std::make_unique<gateway::NullSink>();
    gateway::BoundedForwarder forwarder(config, std::move(sink));

    // Fill queue with agent A
    forwarder.try_forward(make_event("A"));

    // Agent B tries to queue - should fail (queue full), but quota should NOT leak
    auto result = forwarder.try_forward(make_event("B"));
    if (result != gateway::ForwardResult::DroppedQueueFull) return false;

    // B's quota should be 0 (not 1)
    if (forwarder.quota_tracker().in_flight_count("B") != 0) return false;

    // Drain and let B try again
    forwarder.drain_one();
    if (forwarder.try_forward(make_event("B")) != gateway::ForwardResult::Queued) return false;
    if (forwarder.quota_tracker().in_flight_count("B") != 1) return false;

    return true;
}

}  // namespace

int main() {
    // AgentQuotaTracker tests
    if (!test_quota_basic_reserve_release()) {
        std::printf("test_quota_basic_reserve_release failed\n");
        return EXIT_FAILURE;
    }

    if (!test_quota_enforcement()) {
        std::printf("test_quota_enforcement failed\n");
        return EXIT_FAILURE;
    }

    if (!test_quota_multiple_agents()) {
        std::printf("test_quota_multiple_agents failed\n");
        return EXIT_FAILURE;
    }

    if (!test_quota_release_unknown_agent()) {
        std::printf("test_quota_release_unknown_agent failed\n");
        return EXIT_FAILURE;
    }

    // Invariant 1: Bounded backlog
    if (!test_forwarder_bounded_backlog()) {
        std::printf("test_forwarder_bounded_backlog failed\n");
        return EXIT_FAILURE;
    }

    if (!test_forwarder_queue_drains()) {
        std::printf("test_forwarder_queue_drains failed\n");
        return EXIT_FAILURE;
    }

    // Invariant 2: Drop under outage
    if (!test_forwarder_drop_under_backpressure()) {
        std::printf("test_forwarder_drop_under_backpressure failed\n");
        return EXIT_FAILURE;
    }

    if (!test_forwarder_sink_failure_handling()) {
        std::printf("test_forwarder_sink_failure_handling failed\n");
        return EXIT_FAILURE;
    }

    // Invariant 3: Per-agent fairness
    if (!test_forwarder_per_agent_quota()) {
        std::printf("test_forwarder_per_agent_quota failed\n");
        return EXIT_FAILURE;
    }

    if (!test_forwarder_fairness_under_pressure()) {
        std::printf("test_forwarder_fairness_under_pressure failed\n");
        return EXIT_FAILURE;
    }

    if (!test_forwarder_quota_releases_on_drain()) {
        std::printf("test_forwarder_quota_releases_on_drain failed\n");
        return EXIT_FAILURE;
    }

    if (!test_forwarder_quota_tracker_bounded_by_queue()) {
        std::printf("test_forwarder_quota_tracker_bounded_by_queue failed\n");
        return EXIT_FAILURE;
    }

    // Edge cases
    if (!test_forwarder_empty_queue_drain()) {
        std::printf("test_forwarder_empty_queue_drain failed\n");
        return EXIT_FAILURE;
    }

    if (!test_forwarder_queue_full_releases_quota()) {
        std::printf("test_forwarder_queue_full_releases_quota failed\n");
        return EXIT_FAILURE;
    }

    std::printf("All forwarder tests passed\n");
    return EXIT_SUCCESS;
}
