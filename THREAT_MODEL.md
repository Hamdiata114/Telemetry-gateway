# Threat Model

This document describes the security threat model for the Telemetry Ingress Gateway. It identifies threat actors, attack surfaces, and the mitigations implemented at each trust boundary.

## System Context

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              UNTRUSTED ZONE                                 │
│                                                                             │
│   ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐                       │
│   │ Agent A │  │ Agent B │  │ Agent C │  │Attacker │                       │
│   └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘                       │
│        │            │            │            │                             │
│        └────────────┴─────┬──────┴────────────┘                             │
│                           │ UDP                                             │
└───────────────────────────┼─────────────────────────────────────────────────┘
                            │
                            ▼
┌───────────────────────────────────────────────────────────────────────────┐
│                         TRUST BOUNDARY (Network)                          │
└───────────────────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌───────────────────────────────────────────────────────────────────────────┐
│                          GATEWAY PROCESS                                  │
│                                                                           │
│  TB-1: Size Enforcement ──▶ TB-2: Framing ──▶ TB-3: Parsing ──▶          │
│  TB-1.5: Rate Limiting                        TB-4: Validation ──▶       │
│                                               TB-5: Forwarding ──▶ Sink  │
└───────────────────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌───────────────────────────────────────────────────────────────────────────┐
│                         TRUST BOUNDARY (Process)                          │
└───────────────────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌───────────────────────────────────────────────────────────────────────────┐
│                         DOWNSTREAM SYSTEMS                                │
│                    (file, database, message queue)                        │
└───────────────────────────────────────────────────────────────────────────┘
```

## Threat Actors

### 1. Malicious External Attacker
- **Capabilities**: Can send arbitrary UDP packets to the gateway port
- **Goals**: Denial of service, resource exhaustion, data injection
- **Cannot**: Authenticate as a legitimate agent, access internal systems directly

### 2. Compromised Agent
- **Capabilities**: Has valid network access, knows the protocol format
- **Goals**: Overwhelm the system, inject malicious data, starve other agents
- **Cannot**: Exceed their per-agent resource quota

### 3. Misbehaving Agent (Non-Malicious)
- **Capabilities**: Sends malformed data due to bugs
- **Goals**: None (unintentional)
- **Impact**: Should not affect other agents or system stability

## Attack Surfaces

### A. Network Interface (UDP Socket)

| Attack | Description | Mitigation |
|--------|-------------|------------|
| **Packet Flood** | Send millions of packets/sec | TB-1.5: Per-source rate limiting with token bucket |
| **Oversized Packets** | Send packets > MTU | TB-1: Hard size cap at recv(), MSG_TRUNC detection |
| **Fragmentation Attack** | Force IP reassembly | Non-goal: We use `IP_PMTUDISC_DO`, rely on path MTU |
| **Spoofed Source IP** | Fake legitimate agent IP | Accepted risk: UDP has no source verification |

### B. Envelope Framing (TB-2)

| Attack | Description | Mitigation |
|--------|-------------|------------|
| **Length Field Overflow** | Set body_len > actual data | LengthMismatch drop if body_len > available |
| **Trailing Junk** | Extra bytes after declared body | TrailingJunk drop if extra bytes present |
| **Zero-Length Body** | body_len = 0 | Valid but empty; passes to TB-3 which handles |
| **Maximum Length** | body_len = 0xFFFF (65535) | Bounded by TB-1 max datagram size (1472) |

### C. JSON/Logfmt Parsing (TB-3)

| Attack | Description | Mitigation |
|--------|-------------|------------|
| **Deeply Nested JSON** | `{{{{{...}}}}}` | kMaxNestingDepth = 4, drop if exceeded |
| **Huge Arrays** | `[0,0,0,...` millions of elements | kMaxMetrics = 50, drop if exceeded |
| **Long Strings** | Very long agent_id or metric name | Per-field length limits, drop if exceeded |
| **Allocation Bomb** | Request large allocation | No dynamic allocation based on input |
| **Quadratic Parsing** | Pathological backtracking | Single-pass O(n) parser, no backtracking |
| **Invalid UTF-8** | Malformed byte sequences | Treated as opaque bytes, no decoding |

### D. Semantic Validation (TB-4)

| Attack | Description | Mitigation |
|--------|-------------|------------|
| **Future Timestamps** | ts = year 3000 | max_future_ms limit (default: 5 minutes) |
| **Old Timestamps** | ts = year 1970 | max_age_ms limit (default: 1 hour) |
| **Invalid Agent ID** | `agent_id = "../../../etc/passwd"` | Regex validation: `^[a-zA-Z][a-zA-Z0-9_-]{0,63}$` |
| **NaN/Infinity Values** | `"v": NaN` | Explicit rejection of non-finite values |
| **Extreme Values** | `"v": 1e308` | Configurable min/max value bounds |

### E. Forwarding & Backpressure (TB-5)

| Attack | Description | Mitigation |
|--------|-------------|------------|
| **Downstream DoS** | Slow/unavailable downstream | Bounded queue, tail-drop when full |
| **Queue Exhaustion** | Fill queue faster than drain | Fixed max_queue_depth, drops at capacity |
| **Agent Starvation** | One agent fills entire queue | Per-agent quota (max_per_agent) |
| **Memory Exhaustion** | Unbounded backlog growth | Queue bounded by compile-time constant |

## Trust Boundaries in Detail

### TB-1: Receive Loop (Size Enforcement)

**Crossing**: Untrusted network → Gateway process memory

**Invariants**:
- Maximum datagram size: 1472 bytes (enforced at `recvfrom`)
- `MSG_TRUNC` flag checked to detect and drop oversized packets
- `SO_RCVBUF` configured to bound kernel buffer

**Code**: `src/recv_loop.cpp`

### TB-1.5: Source Rate Limiter

**Crossing**: Within process, before expensive parsing

**Invariants**:
- Token bucket per source IP:port
- LRU eviction when max_sources exceeded
- Bounded state: O(max_sources) memory

**Algorithm**:
```
tokens += elapsed_time * tokens_per_sec
tokens = min(tokens, burst_tokens)
if tokens >= 1:
    tokens -= 1
    return Allow
else:
    return Drop
```

**Code**: `src/source_limiter.cpp`

### TB-2: Envelope Parsing

**Crossing**: Raw bytes → Framed message body

**Invariants**:
- Exactly 2-byte header + body, no more, no less
- `body_len` must match actual remaining bytes
- No allocation, returns view into original buffer

**Wire Format**:
```
[0:2]  body_len (big-endian uint16)
[2:N]  body (exactly body_len bytes)
```

**Code**: `src/parse_envelope.cpp`

### TB-3: Content Parsing (JSON/Logfmt)

**Crossing**: Framed bytes → Structured data

**Invariants**:
- All limits are compile-time constants
- Single-pass O(n) parsing
- No dynamic allocation based on input
- Returns views into original buffer

**Limits** (metrics):
| Limit | Value |
|-------|-------|
| kMaxInputBytes | 65536 |
| kMaxAgentIdLen | 64 |
| kMaxMetrics | 50 |
| kMaxMetricNameLen | 128 |
| kMaxTags | 8 |
| kMaxNestingDepth | 4 |

**Code**: `src/parse_metrics.cpp`, `src/parse_log.cpp`

### TB-4: Semantic Validation

**Crossing**: Parsed data → Trusted data

**Invariants**:
- Timestamps within acceptable window
- Agent IDs match safe pattern
- Metric values within bounds
- After TB-4, `agent_id` is trusted for quota purposes

**Validation Rules**:
```
agent_id:  ^[a-zA-Z][a-zA-Z0-9_-]{0,63}$
timestamp: (now - max_age) <= ts <= (now + max_future)
values:    min_value <= v <= max_value, !isnan(v), !isinf(v)
```

**Code**: `src/validate_metrics.cpp`, `src/validate_log.cpp`

### TB-5: Bounded Forwarding

**Crossing**: Validated events → Downstream system

**Invariants**:
- Queue depth bounded by max_queue_depth
- Per-agent in-flight bounded by max_per_agent
- Downstream failure cannot cause unbounded backlog
- Agent quota tracker bounded by queue depth (natural bound)

**Quota Algorithm**:
```
On enqueue:
    if in_flight[agent_id] >= max_per_agent:
        return DroppedAgentQuotaExceeded
    if queue.full():
        return DroppedQueueFull
    in_flight[agent_id]++
    queue.push(event)
    return Queued

On dequeue:
    event = queue.pop()
    in_flight[event.agent_id]--
    if in_flight[event.agent_id] == 0:
        delete in_flight[event.agent_id]  // prune
    sink.write(event)
```

**Code**: `src/forwarder.cpp`

## Security Properties

### Availability
- **Per-source fairness**: No single IP can monopolize processing
- **Per-agent fairness**: No single agent can monopolize queue
- **Bounded resources**: Memory and CPU usage have hard caps
- **Graceful degradation**: Under overload, drops rather than crashes

### Integrity
- **Explicit validation**: Every field validated before trust
- **Type safety**: Enums for drop reasons, not strings
- **No injection**: Agent IDs sanitized, no path traversal possible

### Confidentiality
- **Not addressed**: This is an ingress gateway, assumes network is untrusted
- **Would require**: DTLS for encryption, out of scope

## Accepted Risks

| Risk | Reason Accepted |
|------|-----------------|
| UDP source spoofing | Inherent to UDP; rate limiting by IP mitigates |
| No agent authentication | Out of scope; agent_id is claim, not proof |
| No encryption | Would use DTLS in production |
| Single-process | Demo project; production would use multiple workers |

## Testing Strategy

Each trust boundary has dedicated tests:

| Test File | Coverage |
|-----------|----------|
| `test_recv_loop.cpp` | TB-1: Oversized packets, truncation |
| `test_source_limiter.cpp` | TB-1.5: Rate limiting, LRU eviction |
| `test_parse_envelope.cpp` | TB-2: Framing edge cases |
| `test_parse_metrics.cpp` | TB-3: JSON limits, malformed input |
| `test_parse_log.cpp` | TB-3: Logfmt limits, malformed input |
| `test_validate_metrics.cpp` | TB-4: Timestamps, agent IDs, values |
| `test_validate_log.cpp` | TB-4: Log-specific validation |
| `test_forwarder.cpp` | TB-5: Queue bounds, per-agent quota |
| `test_bounded_queue.cpp` | Queue invariants, drop counting |

## Incident Response

### Symptoms and Responses

| Symptom | Likely Cause | Response |
|---------|--------------|----------|
| High source_limited count | Single IP flooding | Normal; rate limiter working |
| High envelope_drops | Malformed traffic | Check for protocol mismatch |
| High parse_drops | Bad JSON/logfmt | Check agent implementations |
| High validation_drops | Timestamp drift or bad IDs | Check agent clocks/configs |
| High queue_drops | Downstream slow | Scale downstream or tune queue |
| High quota_drops | Agent burst | Normal fairness enforcement |

### Metrics to Monitor

```
received_total
source_limited_total
envelope_drops_total{reason="..."}
parse_drops_total{reason="..."}
validation_drops_total{reason="..."}
queue_drops_total
quota_drops_total
forwarded_total
queue_depth
tracked_agents
tracked_sources
```
