#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace gateway {

// ============================================================================
// Sink Interface
//
// Abstract interface for downstream event consumers.
// Implementations may write to files, network, stdout, or simply discard.
//
// Thread safety: Implementations should document their thread safety.
// ============================================================================

class Sink {
public:
    virtual ~Sink() = default;

    // Write an event payload to the downstream.
    // Returns true on success, false on downstream error.
    //
    // Contract:
    // - May block (e.g., slow filesystem, network)
    // - Should not throw
    [[nodiscard]] virtual bool write(std::span<const std::byte> payload) noexcept = 0;

    // Flush any buffered data (optional operation).
    // Default implementation does nothing.
    virtual void flush() noexcept {}
};

// ============================================================================
// NullSink: Discards all events (for testing/benchmarking)
// ============================================================================

class NullSink final : public Sink {
public:
    [[nodiscard]] bool write(std::span<const std::byte> /*payload*/) noexcept override {
        ++write_count_;
        return true;
    }

    // Metrics for testing
    [[nodiscard]] std::uint64_t write_count() const noexcept { return write_count_; }

private:
    std::uint64_t write_count_ = 0;
};

// ============================================================================
// FailingSink: Always fails (for testing error handling)
// ============================================================================

class FailingSink final : public Sink {
public:
    [[nodiscard]] bool write(std::span<const std::byte> /*payload*/) noexcept override {
        ++fail_count_;
        return false;
    }

    [[nodiscard]] std::uint64_t fail_count() const noexcept { return fail_count_; }

private:
    std::uint64_t fail_count_ = 0;
};

}  // namespace gateway
