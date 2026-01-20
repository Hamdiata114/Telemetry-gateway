# Telemetry Ingress Gateway — Claude Code Instructions

## Goal
This repo is a C++20 UDP telemetry ingress gateway portfolio project focused on systems-security reasoning:
trust boundaries, explicit invariants, bounded resource usage, and observable drop behavior.

## Non-goals
- Reliable delivery / retries
- Agent authentication (agent_id is a claim, not proof)
- Multi-datagram message reassembly
- IP fragmentation reliance

## Hard invariants (do not violate)
- Never allocate memory based on attacker-controlled lengths (payload fields).
- Parsing/validation code must be bounded: no unbounded loops over attacker-controlled structure.
- Strict maximum datagram size (kMaxDatagramBytes). Oversized datagrams must be dropped early.
- One UDP datagram == one message. No buffering/reassembly across datagrams.
- Drop reasons are explicit enums; logs/metrics must not trust agent-provided claims.

## Code structure
- `include/` contains public interfaces (contracts).
- `src/` contains implementation.
- `tests/` contains invariant tests. Every new drop reason or invariant MUST have tests.

## Workflow expectations
- Prefer small commits that implement one invariant at a time.
- Changes to parsing/validation MUST include or update tests.
- Keep the “success path” tested (valid packets must parse and return the correct body span).
- Avoid large refactors without a “no behavior change” commit message and test confirmation.

## Build & test
Configure + build:
- `cmake -S . -B build`
- `cmake --build build`

Run tests:
- `ctest --test-dir build --output-on-failure`

## When editing files
- Before coding, restate the intended invariant(s) and expected drop reason(s).
- After coding, run tests and summarize what changed and which invariants are now enforced.
- If unsure about a security decision, ask before implementing.
