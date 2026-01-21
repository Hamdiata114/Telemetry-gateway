# Telemetry Ingress Gateway

A high-performance UDP telemetry ingress gateway written in C++20, designed with defense-in-depth security principles. This project demonstrates systems-level security reasoning: trust boundaries, explicit invariants, bounded resource usage, and observable drop behavior.

## Overview

```
                    ┌─────────────────────────────────────────────────┐
   UDP Datagrams    │            Telemetry Gateway                    │
   (untrusted)      │                                                 │
        │           │  ┌─────────┐  ┌─────────┐  ┌─────────┐         │
        ▼           │  │  TB-1   │  │  TB-2   │  │  TB-3   │         │
   ┌─────────┐      │  │  Recv   │─▶│Envelope │─▶│  JSON/  │         │
   │ Network │─────▶│  │  Loop   │  │ Parse   │  │ Logfmt  │         │
   └─────────┘      │  └─────────┘  └─────────┘  └─────────┘         │
                    │       │                          │              │
                    │       ▼                          ▼              │
                    │  ┌─────────┐              ┌─────────┐          │
                    │  │ Source  │              │  TB-4   │          │
                    │  │ Limiter │              │Validate │          │
                    │  └─────────┘              └─────────┘          │
                    │                                │               │
                    │                                ▼               │
                    │                          ┌─────────┐          │
                    │                          │  TB-5   │─────────▶│ Sink
                    │                          │Forward  │          │
                    │                          └─────────┘          │
                    └─────────────────────────────────────────────────┘
```

## Features

- **5 Trust Boundaries** — Defense-in-depth with explicit validation at each layer
- **Bounded Resources** — Fixed memory allocation, no attacker-controlled growth
- **Per-Source Rate Limiting** — Token bucket algorithm prevents single-source DoS
- **Per-Agent Fairness** — Quota enforcement prevents agent starvation under load
- **Explicit Drop Reasons** — Every rejection has a typed enum reason for observability
- **Zero-Copy Parsing** — String views into original buffer, no unnecessary copies

## Quick Start

### Build

```bash
cmake -S . -B build
cmake --build build
```

### Run Tests

```bash
ctest --test-dir build --output-on-failure
```

### Run Demo

**Terminal 1 — Start the gateway:**
```bash
./build/gateway_server 9999
```

**Terminal 2 — Generate traffic:**
```bash
./build/traffic_generator 127.0.0.1 9999
```

**Options:**
- `--slow` on server: Adds 100ms delay per write (demonstrates backpressure)
- `--chaos` on generator: Sends malformed packets, bursts, old timestamps

## Architecture

### Trust Boundaries

| Layer | Component | Validates | Drops |
|-------|-----------|-----------|-------|
| **TB-1** | RecvLoop | Datagram size ≤ 1472 bytes | Oversized datagrams |
| **TB-1.5** | SourceLimiter | Per-IP:port rate limit | Rate exceeded |
| **TB-2** | parse_envelope | 2-byte length framing | PayloadTooSmall, LengthMismatch, TrailingJunk |
| **TB-3** | parse_metrics/log | JSON/logfmt structure, field limits | InvalidJson, TooManyMetrics, KeyTooLong, etc. |
| **TB-4** | validate_* | Timestamps, agent_id format, value ranges | TimestampTooOld, AgentIdInvalid, etc. |
| **TB-5** | BoundedForwarder | Queue capacity, per-agent quota | QueueFull, AgentQuotaExceeded |

### Message Formats

**Metrics (JSON):**
```json
{
  "agent_id": "webserver01",
  "seq": 42,
  "ts": 1705689600000,
  "metrics": [
    {"n": "cpu_percent", "v": 65.5, "u": "percent"},
    {"n": "memory_bytes", "v": 1073741824, "tags": {"env": "prod"}}
  ]
}
```

**Logs (logfmt):**
```
ts=1705689600000 level=error agent=webserver01 msg="Connection refused" request_id=req-1234
```

### Wire Protocol

```
┌─────────────────┬────────────────────────────────┐
│ body_len (2B)   │ body (body_len bytes)          │
│ big-endian      │ JSON or logfmt                 │
└─────────────────┴────────────────────────────────┘
```

## Security Invariants

1. **No attacker-controlled allocation** — All buffer sizes are compile-time constants
2. **Bounded parsing work** — O(n) parsing with n bounded by `kMaxInputBytes`
3. **Explicit drop reasons** — Every rejection is a typed enum, never attacker-controlled strings
4. **One datagram = one message** — No reassembly, no buffering across datagrams
5. **Bounded backlog** — Queue depth capped, drops under overload rather than OOM
6. **Per-agent fairness** — No single agent can consume disproportionate resources

## Project Structure

```
telemetry-gateway/
├── include/gateway/       # Public interfaces (contracts)
│   ├── bounded_queue.hpp  # Fixed-capacity queue with tail-drop
│   ├── config.hpp         # Configuration structures
│   ├── forwarder.hpp      # TB-5: Bounded forwarding with quotas
│   ├── parse_envelope.hpp # TB-2: Envelope framing
│   ├── parse_metrics.hpp  # TB-3: JSON metrics parsing
│   ├── parse_log.hpp      # TB-3: Logfmt log parsing
│   ├── recv_loop.hpp      # TB-1: UDP receive with size enforcement
│   ├── sink.hpp           # Downstream sink interfaces
│   ├── source_limiter.hpp # TB-1.5: Per-source rate limiting
│   ├── validate_metrics.hpp # TB-4: Metrics validation
│   └── validate_log.hpp   # TB-4: Log validation
├── src/                   # Implementation
├── tests/                 # Invariant tests
├── demos/                 # End-to-end demo applications
│   ├── gateway_server.cpp # Full pipeline server
│   └── traffic_generator.cpp # Simulated agent traffic
├── THREAT_MODEL.md        # Security threat model
└── CMakeLists.txt
```

## Configuration

### Default Limits

| Parameter | Default | Description |
|-----------|---------|-------------|
| `kMaxDatagramBytes` | 1472 | MTU - IP - UDP headers |
| `kMaxInputBytes` | 65536 | Max JSON/logfmt size |
| `kMaxMetrics` | 50 | Metrics per message |
| `kMaxFields` | 16 | Log fields per message |
| `kMaxAgentIdLen` | 64 | Agent ID length |
| `max_sources` | 1024 | Tracked source IPs (LRU) |
| `tokens_per_sec` | 100 | Per-source rate limit |
| `max_queue_depth` | 4096 | Forwarding queue capacity |
| `max_per_agent` | 64 | Per-agent queue quota |

## Non-Goals

This project intentionally does **not** implement:

- Reliable delivery / retries (UDP is fire-and-forget)
- Agent authentication (agent_id is a claim, not proof)
- Multi-datagram reassembly (one datagram = one message)
- IP fragmentation handling (relies on PMTU discovery)
- Encryption (would use DTLS in production)

## License

MIT

## Author

Portfolio project demonstrating systems security engineering principles.
