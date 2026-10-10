# Shared execution leases

| Field | Value |
|---|---|
| ID | MESHVALE-EXECUTION-001 |
| Version | 0.1.1 |
| Status | Native development interface; no stable API/ABI or release |
| Owner | Geometry |
| Related | [Editing](editing.md), [triangulation](triangulation.md) |

[`execution.h`](../include/meshvale/geometry/execution.h) declares move-only
payload and worker leases linked through installed `meshvale::editing`.
They retain the existing `ExecutionContext` ledger and mutex. The bridge creates
no scheduler, pool or threads; context layout, option order, defaults and
existing algorithm execution policies retain their contracts.

## Payload admission and lifetime

**EXEC-001.** `TryReservePayload(context, bytes, stop)` returns a
`PayloadLeaseResult` with `ReservationStatus::kAccepted`, `kBudgetExceeded` or
`kCanceled`. Accepted admission charges requested bytes and retains a strong
owner of the existing ledger. Rejection returns a detached zero-byte lease.
Context copies, leases and existing computations share one cap. Destroying the
caller context does not invalidate a retained lease.

`Bytes()` reports this lease's charge. `TryResize(total_bytes, stop)` requests an
absolute charge. Growth compares its delta with remaining capacity before
addition; exact-cap admission succeeds without counter overflow. Failed growth
preserves the old charge. Admission does not wait for other charges to release.

**EXEC-002.** Initial admission, including zero bytes, checks cancellation before
and under the mutex. Growth does the same; observed cancellation takes priority
over a budget rejection. A later stop request can race successful admission.
Clients check again before accepting their product result. Shrink, equal-size
resize, reset and destruction ignore cancellation so cleanup remains possible.

Successful zero-byte admission stays ledger-bound and can grow. Default,
moved-from and reset leases are detached: zero resize is an accepted no-op;
positive growth throws `EditorError(kInvalidObject)`. Moved-from contexts throw
that error before admission, including canceled/zero requests. Move assignment
releases its destination's old charge, then transfers ownership. Self-move leaves
the charge intact. Reset is idempotent and detaches.

**EXEC-003.** A lease is a numerical reservation, not an allocation. Clients admit
complete underlying requested blocks before allocation, including their adapter
headers and alignment padding. Check multiplication, size addition and capacity
arithmetic first; client size overflow is distinct from ledger budget rejection.
Keep replacement and old storage charged while both coexist. Allocation failure
unwinds the acquired lease and propagates its original exception.

Free storage before shrinking/releasing its charge. An inline allocation header
can move its lease to a stack local, destroy the header and free the block, then
release the local lease. Stateful allocators own contexts and stop tokens by
value. Shared handles do not double-charge one allocation; independent outputs
need independent leases. The bridge cannot verify client allocation policy.

## Worker admission and joins

**EXEC-004.** `TryReserveWorkers(context, desired)` atomically reserves the smaller
of desired and available slots. Fewer than two slots yields zero `Count()` for
caller-serial fallback, matching bounds/triangulation policy. It does not wait,
launch threads or create a nested pool. Moved-from contexts throw `kInvalidObject`.

Worker leases retain the ledger, support move construction/assignment and
idempotent reset, and release slots on destruction. Count is reservations, not
successfully created workers. Hold the lease until every launched worker joins,
including cancellation, worker exceptions and partial-launch failure. Declare
the lease before scope-based worker owners so those owners unwind first. No
launch, join, callback, allocation or user cleanup runs under the ledger mutex.

Product operations own partitioning, thresholds, stop checks, deterministic
assembly and actual joined-worker telemetry. Held slots constrain existing calls
sharing the context; insufficient capacity causes their documented caller
fallback. External caller threads remain outside this cap. No throughput claim
follows from slot admission.

## Metrics and safety boundary

**EXEC-005.** `ActiveTrackedPayload()` sums declared reservations of active
computations and explicitly leased requested allocations. Lifetime
`PeakTrackedPayload()` observes that shared total, not process heap or RSS.
`TrackedPayloadBudget()` retains its 256 MiB default and existing two-field
option initializers remain valid. Bounds reserve slots and no payload.
Triangulation retains its [own accounting exclusions](triangulation.md#declared-payload-budget);
its returned Mesh lifetime stays outside active accounting. Arbitrary old
objects and Python raw-record buffers are not automatically leased.

`ActiveWorkers()` and lifetime `PeakWorkers()` include public slot leases.
Algorithms report actual created/joined threads separately. System heap overhead,
stacks, caller inputs, unrelated runtime memory and exception/thread runtime
remain exclusions. This is no global heap guard or process memory limit.

**EXEC-006.** Normal acquisition, resize, move, reset and destruction allocate
nothing and reuse the existing shared-owner control block. Detached cleanup skips
ledger access. Separate leases/context copies are safe for concurrent use;
operations on the same lease, or moving/destroying a context alongside access to
that C++ object, need external synchronization. All admission/counters use the
existing mutex. No callback runs under it.

Standard mutex/runtime exceptions may propagate from admission/resize. Noexcept
cleanup requires a valid ledger mutex; it does not recover corrupted objects or
mutex failure. Genuine allocation/thread/runtime exceptions retain their owning
operation contracts. Leases do not make uncharged cleanup safe.

`SharesExecutionBudget(first, second) noexcept` compares ledger ownership without
allocation, locking or exposing a raw identity. It is true only when both
contexts are valid and share the same worker/payload ledger. Context copies match;
independent contexts with equal options do not. Either moved-from context yields
false, including comparison of two moved-from contexts. Stateful rebound
allocators can use this predicate for equality. Stop-token equality is separate
from ledger identity: allocator stop values govern future admission, while each
inline lease owns the ledger needed for deallocation.

## Installed usage and verification

Follow [native build/install steps](attributes.md#build-and-installed-consumer).
The [installed execution consumer](../examples/execution-consumer/CMakeLists.txt)
includes the canonical header independently and links exported targets only. It
demonstrates retained context lifetime, explicit charges and real joined workers
with nested bounds fallback. Native tests cover state/move/cancellation behavior,
exact/max-size admission, overlap, allocation-failure rollback, allocation-free
normal lease paths, concurrent mixed reservations and existing algorithm
contention. Sanitizer coverage requires executing that build; a configured job
or functional concurrency test is not race-detector evidence.

The default native build and address/undefined-behavior sanitizer job run twelve
test suites. Three separate allocation-failure executables replace ordinary
throwing scalar/array `new` and sized/unsized `delete`; their replacement bodies
remain in a separate compilation unit without CMake interprocedural
optimization. They check real allocation rejection and rollback for shared
leases, attribute capture/result storage and atomic editor batches, as well as
allocation-free lease paths and snapshot traversal. Aligned allocation and
thread/exception runtime are outside these focused probes.

ThreadSanitizer owns its own allocation interceptors. Its job explicitly sets
`MESHVALE_GEOMETRY_ALLOCATION_PROBE=OFF`, runs nine suites and retains all shared
lease state/lifetime, worker, algorithm-contention and concurrency cases. The
replacement-allocation probes do not run under ThreadSanitizer; they remain
enabled in ordinary and address/undefined-behavior builds. The
[test inventory check](../scripts/check-native-tests.py) verifies each job's
exact selected suites before execution.
