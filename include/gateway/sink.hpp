#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <thread>

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

// ============================================================================
// StdoutJsonSink: Prints JSON payloads to stdout (for demos)
// ============================================================================

class StdoutJsonSink final : public Sink {
public:
    [[nodiscard]] bool write(std::span<const std::byte> payload) noexcept override {
        // Convert bytes to string and print
        std::fwrite(payload.data(), 1, payload.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        ++write_count_;
        return true;
    }

    void flush() noexcept override {
        std::fflush(stdout);
    }

    [[nodiscard]] std::uint64_t write_count() const noexcept { return write_count_; }

private:
    std::uint64_t write_count_ = 0;
};

// ============================================================================
// SlowSink: Simulates slow downstream (for backpressure demos)
//
// Wraps another sink and adds artificial delay per write.
// ============================================================================

class SlowSink final : public Sink {
public:
    // delay_ms: milliseconds to sleep before each write
    explicit SlowSink(std::unique_ptr<Sink> inner, std::uint32_t delay_ms)
        : inner_(std::move(inner))
        , delay_(delay_ms) {}

    [[nodiscard]] bool write(std::span<const std::byte> payload) noexcept override {
        std::this_thread::sleep_for(delay_);
        return inner_->write(payload);
    }

    void flush() noexcept override {
        inner_->flush();
    }

private:
    std::unique_ptr<Sink> inner_;
    std::chrono::milliseconds delay_;
};

}  // namespace gateway
