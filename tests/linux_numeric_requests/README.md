# Linux original numerical request observation

This standalone test observes the compiled `ScalarBuffer` owners for all seven
canonical types and the compiled double XYZ `VectorBuffer` owner. It compares
actual original allocation arguments and free order with their private compiled
request plans and shared-ledger admission. It does not qualify complete mesh
materialization or enable a production allocation profile.

The test deliberately uses the private request/fill seam, independently of
public admission. The TSan profile remains unqualified; the observed native and
ASan/UBSan profile has a bounded admission contract in
[materialization](../../docs/materialization.md#compiled-allocation-profiles).
Do not change the production predicate to make this observer pass. Profile
admission requires reviewed observations followed by separate public factory,
worker, lifetime and failure qualification.

## Matching build and invocation

Use Linux x64 Clang, Debug, and an exact installed Geometry source/binary pair.
The observer needs that source's private declarations; it does not need Eigen
headers. Use separate fresh builds and installed prefixes for native and matching
Clang ASan/UBSan or TSan. Actual Geometry owner compilation still requires pinned Eigen
3.4.1. Preserve the dependency archive and installed-library hashes, compiler and
linker versions, compile commands, linker map, disassembly/relocations, raw
stdout/stderr and checked command exit codes.

For a serial original-request prerequisite, configure Geometry with
`BUILD_TESTING=OFF`; the full factory suites run separately under their explicit
supported-profile gates. An unqualified profile needs separately reviewed
admission before those suites can succeed. This observer does not replace them.

```sh
cmake -S "$GEOMETRY_SOURCE" -B "$GEOMETRY_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DBUILD_TESTING=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$GEOMETRY_BUILD" --parallel 2
cmake --install "$GEOMETRY_BUILD" --prefix "$GEOMETRY_PREFIX"
python "$OBSERVER_SOURCE/capture-profile.py" --source "$GEOMETRY_SOURCE" --compile-commands "$GEOMETRY_BUILD/compile_commands.json" --revision "$GEOMETRY_SHA" --profile native --output "$OWNER_PROFILE_OUTPUT"
cmake -S "$OBSERVER_SOURCE" -B "$OBSERVER_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH="$GEOMETRY_PREFIX" -DOBSERVER_PROFILE=native -DOBSERVER_GEOMETRY_SOURCE="$GEOMETRY_SOURCE" -DOBSERVER_GEOMETRY_REVISION="$GEOMETRY_SHA" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$OBSERVER_BUILD" --parallel 2
"$OBSERVER_BUILD/observer" > "$TRACE_FILE"
python "$OBSERVER_SOURCE/check-trace.py" "$TRACE_FILE" "$OWNER_PROFILE_OUTPUT/profile.json" "$REQUEST_REPORT"
```

Run the checker only after the observer exits successfully. Redirection does not
establish successful execution. The capture script creates a new output directory
and preserves preprocessing failures. The checker creates a new report and
refuses to replace an existing one.

For the ASan/UBSan cohort, compile and link both Geometry and observer with
`-fsanitize=address,undefined -fno-omit-frame-pointer`, selecting observer profile
`asan-ubsan`. Use `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Keep all wrappers normally
instrumented and errors unsuppressed. TSan real-worker/cap tests run separately;
the serial allocation prerequisite cannot qualify those workers.

For the separate TSan prerequisite, select `OBSERVER_PROFILE=tsan` and compile
every Geometry and observer translation unit with
`-fsanitize=thread -O1 -g -fno-omit-frame-pointer`, linking both with
`-fsanitize=thread`. Capture with `--profile tsan`; run with
`TSAN_OPTIONS=halt_on_error=1`. The effective owning optimization must be `-O1`,
debug information and frame pointers must remain enabled, and mixed/disabled
sanitizers, ignorelists and opaque frontend overrides are rejected. Wrappers and
the recorder remain fully instrumented. The actual observer must report
`ASAN=0/TSAN=1`; original references, disassembly and runtime identities must
establish delegation to the matching TSan originals. Retain all runtime failures
without suppression. This prerequisite uses the private seam and requires the
compiled plans to report unqualified TSan admission; it never enables the
production profile. After reviewed actual route observations and a separate
profile admission, the mandatory allocation-probe-OFF 13-suite TSan gate must
execute full factories, real workers, cancellation, partial launches, shared
caps and lifetime tests. Serial allocation observations cannot replace it.

## Observation contract

The 59 cases cover original-function calibration, charged accounting-seed
construction/destruction and one-byte-less seed rejection, then eight numerical
owners at counts zero, one, seven and seventeen. Every numerical owner runs at
its exact compiled new-operation cap; each nonempty owner also checks rejection
at one byte less without starting an original numerical request.

Test-only GNU linker wrappers delegate to the original C malloc/free/calloc/
realloc and Itanium scalar/array/aligned new/delete symbols. They do not define
replacement global operators or change a production allocator. Fixed-capacity
serial logging performs no allocation, formatting or thread-local interception
inside a call. It records root and operation charges before each original
allocation and before/after each original free. Formatting occurs after the
observation scope closes. Overflow or an unexpected route fails qualification.

Numeric lease admission precedes both the real PIMPL new and real Eigen malloc.
The original Eigen owner is destroyed before its lease: observed free callbacks
must still see both requests charged. The subsequent ledger sample must restore
the seed charge. Zero shapes request neither a PIMPL nor numerical block. The
seed trace independently matches its full aligned original-new request argument
to its pre-call root charge, including compiled header and padding.

The checker compares each original numerical malloc argument with the compiled
plan and an independent payload/padding calculation based on actual owning
translation-unit macros. It also checks numerical data alignment and original
malloc-pointer correspondence, balanced free routes, exact-cap admission and
one-byte-less rollback. Caller-supplied source identities and source hashes
remain separate from installed-binary provenance; the invocation owner must
capture that provenance independently.

Pinned Eigen construction may call `free(nullptr)` while replacing its initially
empty storage. The checker retains the matched begin/end events in its report
and validates their zero arguments, full charges and construction phase between
PIMPL creation and numerical malloc. That call cannot discharge a live block or
replace the later required numerical free and PIMPL delete. Unexpected null
allocations, null calls in other phases and missing/nonzero frees still fail.

`capture-profile.py` replays preprocessing with the scalar/vector owners' exact
compile commands, retaining original flags and source hashes. It never redefines
Eigen macros. It rejects sanitizer disabling/ignorelist flags and opaque
response/config inputs rather than inferring effective instrumentation from
positive flags alone. Observer-side sanitizer features describe the observer only. They
are not a substitute for these compiled-owner macro records.

[GNU linker wrapping](https://sourceware.org/binutils/docs/ld/Options.html) affects
undefined references. Shared-library internals or elided calls may bypass it;
retain the actual object relocations and final link map. C and C++ call layers
remain separate: never sum nested observations into a process heap or RSS claim.
No source-local Linux execution or throughput claim follows from preparing or
synthetically checking this harness.
