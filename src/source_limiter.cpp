#include "gateway/source_limiter.hpp"

#include <algorithm>  // std::min

namespace gateway {

SourceLimiter::SourceLimiter(SourceLimiterConfig config, Clock clock)
    : config_(config)
    , clock_(std::move(clock)) {}

Admit SourceLimiter::admit(const SourceKey& source) {
    auto now = clock_();

    auto it = sources_.find(source);
    if (it == sources_.end()) {
        // New source: evict if at capacity
        if (sources_.size() >= config_.max_sources) {
            evict_lru();
        }

        // Create new entry with full bucket
        lru_list_.push_front(source);
        Entry entry{
            .bucket = Bucket{
                .tokens = static_cast<double>(config_.burst_tokens),
                .last_update = now
            },
            .lru_pos = lru_list_.begin()
        };
        it = sources_.emplace(source, entry).first;
    } else {
        // Existing source: move to front of LRU
        lru_list_.erase(it->second.lru_pos);
        lru_list_.push_front(source);
        it->second.lru_pos = lru_list_.begin();
    }

    // Refill tokens based on elapsed time
    refill_bucket(it->second.bucket);

    // Try to consume a token
    if (it->second.bucket.tokens >= 1.0) {
        it->second.bucket.tokens -= 1.0;
        ++total_admits_;
        return Admit::Allow;
    }

    ++total_drops_;
    return Admit::Drop;
}

void SourceLimiter::refill_bucket(Bucket& bucket) {
    auto now = clock_();
    auto elapsed = std::chrono::duration<double>(now - bucket.last_update);
    double tokens_to_add = elapsed.count() * config_.tokens_per_sec;

    bucket.tokens = std::min(
        bucket.tokens + tokens_to_add,
        static_cast<double>(config_.burst_tokens)
    );
    bucket.last_update = now;
}

void SourceLimiter::evict_lru() {
    if (lru_list_.empty()) {
        return;
    }
    // Remove least recently used (back of list)
    const SourceKey& victim = lru_list_.back();
    sources_.erase(victim);
    lru_list_.pop_back();
    ++eviction_count_;
}

std::size_t SourceLimiter::tracked_count() const noexcept {
    return sources_.size();
}

bool SourceLimiter::is_tracked(const SourceKey& source) const {
    return sources_.find(source) != sources_.end();
}

}  // namespace gateway
