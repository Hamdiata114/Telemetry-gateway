#include "gateway/forwarder.hpp"

namespace gateway {

// ============================================================================
// AgentQuotaTracker Implementation
// ============================================================================

AgentQuotaTracker::AgentQuotaTracker(std::size_t max_per_agent) noexcept
    : max_per_agent_(max_per_agent) {}

bool AgentQuotaTracker::try_reserve(const std::string& agent_id) noexcept {
    auto& count = in_flight_[agent_id];  // Inserts 0 if new
    if (count >= max_per_agent_) {
        ++quota_rejections_;
        return false;  // Reject: agent over quota
    }
    ++count;
    ++total_in_flight_;
    return true;
}

void AgentQuotaTracker::release(const std::string& agent_id) noexcept {
    auto it = in_flight_.find(agent_id);
    if (it != in_flight_.end()) {
        if (it->second > 0) {
            --it->second;
            --total_in_flight_;
        }
        // Prune entry if count reaches 0 (keeps map bounded)
        if (it->second == 0) {
            in_flight_.erase(it);
        }
    }
}

std::size_t AgentQuotaTracker::in_flight_count(const std::string& agent_id) const noexcept {
    auto it = in_flight_.find(agent_id);
    return it != in_flight_.end() ? it->second : 0;
}

std::size_t AgentQuotaTracker::tracked_agents() const noexcept {
    return in_flight_.size();
}

std::size_t AgentQuotaTracker::total_in_flight() const noexcept {
    return total_in_flight_;
}

// ============================================================================
// BoundedForwarder Implementation
// ============================================================================

BoundedForwarder::BoundedForwarder(ForwarderConfig config, std::unique_ptr<Sink> sink)
    : config_(config)
    , quota_tracker_(config.max_per_agent)
    , queue_(config.max_queue_depth)
    , sink_(std::move(sink)) {}

ForwardResult BoundedForwarder::try_forward(QueuedEvent event) noexcept {
    // Capture agent_id before any move (needed for quota release on failure)
    const std::string agent_id = event.agent_id;

    // Step 1: Check agent quota (fairness)
    if (!quota_tracker_.try_reserve(agent_id)) {
        ++dropped_quota_;
        return ForwardResult::DroppedAgentQuotaExceeded;
    }

    // Step 2: Try to enqueue (backlog bound)
    if (queue_.try_push(std::move(event)) == PushResult::Dropped) {
        // Must release the quota we just reserved since enqueue failed
        quota_tracker_.release(agent_id);
        ++dropped_queue_full_;
        return ForwardResult::DroppedQueueFull;
    }

    return ForwardResult::Queued;
}

bool BoundedForwarder::drain_one() noexcept {
    auto event_opt = queue_.try_pop();
    if (!event_opt) {
        return false;  // Queue was empty
    }

    QueuedEvent& event = *event_opt;

    // Release agent quota (must happen regardless of sink success)
    quota_tracker_.release(event.agent_id);

    // Write to sink
    if (sink_->write(std::span<const std::byte>(event.payload))) {
        ++total_forwarded_;
    } else {
        ++sink_failures_;
    }

    return true;
}

std::size_t BoundedForwarder::drain_all() noexcept {
    std::size_t count = 0;
    while (drain_one()) {
        ++count;
    }
    return count;
}

std::size_t BoundedForwarder::queue_depth() const noexcept {
    return queue_.size();
}

std::size_t BoundedForwarder::queue_capacity() const noexcept {
    return queue_.capacity();
}

bool BoundedForwarder::queue_empty() const noexcept {
    return queue_.empty();
}

const AgentQuotaTracker& BoundedForwarder::quota_tracker() const noexcept {
    return quota_tracker_;
}

}  // namespace gateway
