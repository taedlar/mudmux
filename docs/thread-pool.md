# Thread Pool Refactor Plan

## Purpose

This document defines a phased plan for extending mudmux with optional thread-pool execution while preserving the original deterministic behavior when configured appropriately.

The thread pool is configured through `mudmux_init()` via YAML config.

## Behavioral Contract

### Determinism Modes

- `thread_pool_size: 1`
  - Preserves the original determinism model.
  - Hook execution order and side-effect ordering remain equivalent to single-thread event-loop behavior.

- `thread_pool_size: N` where `N > 1`
  - Enables parallel processing for higher throughput.
  - Determinism is not guaranteed across independent tasks/slots.
  - Safety invariants must still hold (no data races, no deadlocks, valid slot lifetime handling).

### Backward Compatibility

- Default value must be `thread_pool_size: 1`.
- Existing deployments that do not set thread pool size keep current behavior.
- Startup logs should explicitly state whether deterministic mode or relaxed mode is active.

## Configuration in mudmux_init

## YAML shape

```yaml
transport:
  thread_pool:
    size: 1
```

### Parsing Rules

- Parse in `mudmux_init(const char* config_yaml)`.
- If missing: use default `size = 1`.
- If `size < 1`: reject configuration and fail init.
- If `size == 1`: deterministic mode.
- If `size > 1`: relaxed mode (non-deterministic ordering allowed).

### Runtime State

Introduce explicit runtime policy values:

- `thread_pool_size` (integer)
- `determinism_mode` (derived enum):
  - `DETERMINISM_STRICT` when size is 1
  - `DETERMINISM_RELAXED` when size is greater than 1

## Architecture Extension

The thread-pool model is an extension of the current lock-and-hook model, not a replacement.

### Keep Streamlined Main Loop

Main event loop remains authoritative for:

- inbound refill
- outbound flush
- runtime registration updates (`async_runtime_add/modify/remove`)

### Parallelizable Work

Thread pool can process compute-heavy tasks and slot-local transformations where safe.

If work item side effects must touch comm state, the side-effect APIs acquire per-slot lock internally.

## Locking Model

### Lock Domains

1. Slot-table lock
   - Protects slot map/storage operations (add/remove/resize/lookup metadata).

2. Per-slot lock
   - Protects mutable state for one slot (flags, inbound/outbound queues, TLS/TELNET state, mode transitions).

3. Global pool locks
   - Dedicated mutex for inbound buffer pool.
   - Dedicated mutex for outbound buffer pool.

4. Hook serialization lock
   - Preserved for strict deterministic mode semantics.
   - Can be conditionally relaxed only under documented relaxed mode behavior.

### Lock Rules

- Never hold one slot lock while waiting on another slot lock.
- Never invoke hooks while holding a slot lock.
- Keep pool lock sections short and local to pool push/pop operations.
- Use a single lock order when multiple lock classes are needed:
  - slot-table -> slot -> pool

## API Contract Migration: Pointer to Slot-ID

Current pointer-escaping API patterns are unsafe for parallel evolution.

### Goal

Replace pointer-based external contracts with slot-id based contracts.

### Migration Steps

1. Add slot-id APIs for all side-effect operations:
   - write/buffered_write by slot
   - close by slot
   - mode changes by slot
   - telnet/tls controls by slot
   - lightweight state query by slot

2. Deprecate external pointer-return usage in application-facing code.

3. Keep temporary compatibility wrappers during transition.

4. Remove pointer-escaping contract at major-version boundary.

### Compatibility Recommendation

- In strict mode (`size=1`), preserve legacy behavior while deprecations are introduced.
- In relaxed mode (`size>1`), prefer slot-id APIs only; avoid new pointer-based usage.

## Phased Plan

## Phase 0: Contract and Instrumentation

- Document strict vs relaxed determinism semantics.
- Add startup log line including pool size and active determinism mode.
- Add debug assertions for lock-order violations.

Exit criteria:

- Behavior contract published and review-approved.
- Runtime logs clearly show selected mode.

## Phase 1: Configuration Plumbing

- Parse `transport.thread_pool.size` in `mudmux_init()`.
- Store `thread_pool_size` and derived `determinism_mode` in runtime configuration.
- Validate input range.

Exit criteria:

- Invalid values fail fast.
- Missing config defaults to strict mode (`size=1`).

## Phase 2: Lock Domain Split

- Introduce slot-table lock and per-slot lock.
- Move side-effect API synchronization from global lock to per-slot locks.
- Keep main-loop refill/flush path intact.

Exit criteria:

- No global serialization bottleneck for slot-local side effects.
- Existing tests pass with `size=1`.

## Phase 3: Dedicated Pool Mutexes

- Add mutex for inbound recycle pool.
- Add mutex for outbound recycle pool.
- Ensure lock-order rules are enforced.

Exit criteria:

- No data race in pool allocation/recycle under stress.

## Phase 4: Slot-ID API Adoption

- Introduce and prefer slot-id write/query APIs.
- Migrate examples and tests to slot-id usage.
- Mark pointer-based APIs as deprecated.

Exit criteria:

- Core examples compile and run without pointer-return dependency.

## Phase 5: Thread Pool Execution Policy

- Add worker pool init/deinit based on `thread_pool_size`.
- In strict mode: single worker semantics preserve deterministic ordering.
- In relaxed mode: allow concurrent work execution with documented non-determinism.

Exit criteria:

- Throughput improves in relaxed mode benchmarks.
- Strict mode regression tests maintain ordering equivalence.

## Phase 6: Hardening and Removal

- Remove or fence remaining pointer-escaping paths.
- Finalize API docs and migration notes.
- Add stress suites for deadlock/race detection.

Exit criteria:

- TSAN/helgrind-style runs show no races in comm layer.
- Deadlock stress tests pass.

## Testing Plan

### Strict Mode (`size=1`)

- Ordering-sensitive tests assert stable hook/event ordering.
- Regression tests compare against pre-refactor behavior.

### Relaxed Mode (`size>1`)

- Safety-first assertions:
  - no crashes
  - no deadlocks
  - no slot lifetime violations
  - correct eventual outcomes
- Avoid tests that require exact global ordering.

## Open Questions

- Should per-slot ordering remain strict in relaxed mode while cross-slot ordering is relaxed?
- Should hook serialization lock be always enabled in strict mode and partially relaxed in relaxed mode?
- Should pointer-return APIs be compile-time disabled under a feature flag once slot-id migration is complete?

## Recommended Defaults

- Default config: `thread_pool_size: 1`
- Default mode: strict deterministic
- Explicit log warning when `thread_pool_size > 1`:
  - deterministic ordering not guaranteed
