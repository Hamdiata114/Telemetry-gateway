# Threat Model

## Trust Boundaries
- TB-1: Network → Gateway
- TB-2: Parsing → Validation
- TB-3: Validation → Queue
- TB-4: Queue → Downstream
- TB-5: Observability
- TB-6: Control Plane

## Current Implementation Focus
- TB-1
- TB-2

## Non-Goals (for now)
- Reliable delivery
- Agent authentication
- Downstream backpressure handling



The goal of this project is to accept data from agents and forward only validated, bounded, and well-formed data to internal services, while staying available under malformed or malicious inputs. The system most continue accept request under malformed input. It does not crash or leak memory. 

The following must be true, 
    • The gateway must remain available to other agents.
    • Integrity of data sent to the internal services 
    • Fairness, all agents must be able to send data within a reasonable amount of time.

In scope would be the following: In scope: input validation, rate limiting, overload behavior, downstream outage behavior.

Actors
    • Legitimate agent: sends well-formed UDP datagrams at an expected cadence.
    • Buggy agent: accidentally sends oversized datagrams, malformed JSON, bursts/retry storms.
    • Compromised agent (insider): can send from “valid” network locations but behaves maliciously (flooding, poisoning, spoofing other agents).
    • Off-path attacker: can reach your UDP port and flood garbage; may spoof source IPs (depends on network).
    • Downstream sink/service: trusts gateway output; can be slow or down.

Entry points
    • UDP ingest socket (host:port): the primary entry point.
    • Gateway configuration surface: local config, flags, env vars (misconfig → broken limits, open exposure).
    • Logging/metrics: attacker-controlled content can pollute logs; metrics cardinality can explode.
    • Gateway → downstream connection: can fail, stall, or back up.
    
Trust boundary
    • Network -> UDP socket
        ○ Threats
            § Spoofing
                □ Because UDP provides weak sender identity, an attacker may spoof source addresses/ports (or appear as many distinct senders), undermining per-agent fairness controls and potentially starving legitimate agents.
            § DOS
                □ An attacker can send high-rate UDP traffic that exceeds the gateway’s processing capacity, causing resource exhaustion (CPU time, memory, or receive buffers), leading to widespread packet drops, increased latency, or process failure.
        
        ○ Invariants
            § MUST bound per-source share of processing capacity using observable network attributes.
            
            § MUST bound total work so overload results in controlled drops rather than unbounded latency/resource growth.
            
            § MUST avoid unbounded per-sender state growth
    • Datagram Bytes → Message Interpretation'
        ○ Threats
            § Because the gateway treats each UDP datagram as a complete telemetry message, an attacker can send oversized datagrams (or payloads near the gateway’s max accepted size), causing disproportionate CPU/memory overhead per packet and increasing packet loss/delay for other agents, degrading availability and fairness.
            § If the gateway receives a datagram larger than its receive buffer (or otherwise corrupted), it may process a truncated/partial message, leading to parse failures and wasted work; if truncation isn’t handled strictly, it risks forwarding malformed data or causing inconsistent behavior.
                    
        
        ○ Invariants
            § MUST enforce a strict maximum accepted datagram size; oversized datagrams are dropped at TB-2 and never reach parsing.
            § MUST interpret one UDP datagram as one telemetry message (no multi-datagram messages).
            § MUST not rely on IP fragmentation; accepted messages must fit within the configured maximum such that fragmentation is not required.
    • Bytes → Parsed JSON
        ○ Threats
            § If the gateway receives a datagram larger than its receive buffer (or otherwise corrupted), it may process a truncated/partial message, leading to parse failures and wasted work; if truncation isn’t handled strictly, it risks forwarding malformed data or causing inconsistent behavior.
            
            § At TB-3, the gateway assumes that JSON parsing cost is proportional to input size. An attacker can send valid but deeply nested or complex JSON that is expensive to parse, monopolizing CPU and delaying processing of other agents’ messages, degrading availability and fairness.”
        
        ○ Invariants
            § MUST ensure decoding/parsing a single message cannot cause unbounded memory allocation; memory consumed per message is bounded.
            § MUST ensure parsing work per message is bounded; one message must not monopolize CPU such that other messages are starved.
            § Yes, no partial parse results cross TB-3
    • Parsed JSON → Validated/Normalized “Event”
        ○ Threats
            § If agent identity is asserted in the payload, an attacker can spoof agent_id to inject falsified telemetry attributed to a legitimate agent, poisoning downstream data and potentially causing misattribution or unfair
            § Attackers can send syntactically valid JSON with semantically invalid types/values to trigger validation failures, increasing processing overhead and potentially exploiting coercion or edge cases if validation is inconsistent. throttling.
        ○ Invariants
            § MUST enforce semantic validity constraints on well-typed fields (e.g., timestamps within an acceptable window; numeric values within configured ranges) before an event is accepted/forwarded.
            §  MUST validate that all required fields are present and correctly typed; events failing contract validation are rejected and never forwarded.
            § MUST normalize all accepted events into a canonical, well-defined format before forwarding, so downstream systems never see multiple representations of the same logical event.
            § MUST require an agent_id field to be present and conform to an allowed format; the agent_id is treated as a claim, not proof of identity.
            
        
        
    •  Validated Event → Downstream
        ○ Threats
            § If the downstream system is slow or unavailable, the gateway may accumulate validated events in internal queues or buffers. As these resources fill, the gateway’s ability to accept and process new telemetry degrades, violating availability guarantees and potentially causing unfair starvation of other agents.
            § Downstream failure propagates backward and causes internal resource exhaustion and/or fairness collapse.
        ○ Invariants
            § MUST ensure downstream slowness/outage cannot cause unbounded backlog growth or unbounded processing work in the gateway (resource use remains bounded).'
            § MUST not allow downstream failure to force unbounded buffering; under downstream outage it is acceptable to drop events rather than accumulate backlog
            § MUST prevent any single agent (as identified after TB-4 validation) from consuming a disproportionate share of downstream/backlog capacity, starving other agents during downstream degradation.
            
    • Event Content → Logs and Metrics
        ○ Threats
            § Attacker-controlled telemetry content can produce log entries that mislead operators or monitoring systems, obscuring real failures or triggering false investigations.
            
            § Attacker-controlled telemetry values can distort metrics used for health assessment or automation, leading to false alerts, missed failures, or incorrect operational decisions.
            § At TB-6, telemetry data is assumed to be valid and authoritative. If an attacker poisons logs or metrics, downstream consumers (humans or automation) may make incorrect operational decisions based on trusted but false information, causing systemic harm.
        ○ Invariants
            § Metrics MUST reflect gateway health and behavior, and MUST remain usable for alerting even under malicious or high-volume telemetry input (i.e., no single agent or input pattern can distort or drown out gateway health signals).
            § Logs MUST record gateway-controlled actions and state transitions (e.g., drops, validation failures, downstream availability changes), and MUST NOT allow attacker-controlled telemetry content to obscure or impersonate gateway behavior.
            







    
