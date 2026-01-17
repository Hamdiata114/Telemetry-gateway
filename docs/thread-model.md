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
