# SPDX-License-Identifier: Apache-2.0
"""Check original request arguments, compiled plans, charges and free order."""
import argparse
import json
from pathlib import Path

SHAPES = {"scalar-f32": (4, 1), "scalar-f64": (8, 1), "scalar-i32": (4, 1),
          "scalar-u8": (1, 1), "scalar-u16": (2, 1), "scalar-u32": (4, 1),
          "scalar-u64": (8, 1), "vector-f64x3": (8, 3)}
COUNTS = {0, 1, 7, 17}
ALLOCATIONS_C = {"malloc", "calloc", "realloc"}
ALLOCATIONS_NEW = {"new", "new-array", "new-aligned", "new-array-aligned"}
DELETIONS = {"delete", "delete-array", "delete-sized", "delete-array-sized",
             "delete-aligned", "delete-array-aligned", "delete-sized-aligned",
             "delete-array-sized-aligned"}
PHASES = {"calibration", "seed", "seed-destroy", "admitted", "construct",
          "destroy", "owner-freed", "lease-freed", "denied"}

def macro_int(macros, name):
    seen = set()
    value = name
    while value in macros:
        assert value not in seen, "Cyclic macro aliases"
        seen.add(value)
        value = macros[value].strip().strip("()")
    return int(value, 0)

def parse(text):
    profile, sources, cases, current = None, None, [], None
    for line in text.splitlines():
        fields = line.split("\t")
        if fields[0] == "PROFILE":
            assert profile is None and len(fields) == 7 and fields[1] == "1"
            profile = fields
        elif fields[0] == "SOURCES":
            assert sources is None and len(fields) == 4
            sources = fields[1:]
        elif fields[0] == "CASE":
            assert len(fields) == 15
            current = dict(zip(("shape", "mode"), fields[1:3]))
            names = ("scalar_bytes", "width", "count", "impl", "numeric", "qualified",
                     "event_count", "root_before", "op_before", "root_after", "op_after", "data")
            current.update(zip(names, map(int, fields[3:])))
            current["events"] = []
            cases.append(current)
        elif fields[0] == "EVENT":
            assert current is not None and len(fields) == 11
            event = {"ordinal": int(fields[1]), "phase": fields[2],
                     "kind": fields[3], "action": fields[4]}
            event.update(zip(("first", "second", "pointer", "previous", "root", "op"), map(int, fields[5:])))
            assert event["ordinal"] == len(current["events"])
            current["events"].append(event)
        else:
            raise AssertionError("Unknown trace record: " + line)
    assert profile is not None and sources is not None
    return profile, sources, cases

def verify(text, owners):
    profile, sources, cases = parse(text)
    assert profile[2] == owners["profile"] and profile[3] == owners["revision"]
    assert profile[5] == ("ASAN=1" if profile[2] == "asan-ubsan" else "ASAN=0")
    assert profile[6] == "TSAN=0"
    assert sources == [owners["owners"]["scalar_buffer"]["source_sha256"],
                       owners["owners"]["vector_buffer"]["source_sha256"],
                       owners["allocation_source_sha256"]]
    routes = []
    for owner in ("scalar_buffer", "vector_buffer"):
        record = owners["owners"][owner]
        assert record["exit_code"] == 0
        macros = record["macros"]
        assert tuple(macro_int(macros, k) for k in ("EIGEN_WORLD_VERSION", "EIGEN_MAJOR_VERSION", "EIGEN_MINOR_VERSION")) == (3, 4, 1)
        assert macro_int(macros, "__linux__") == 1 and macro_int(macros, "__LP64__") == 1
        routes.append((macro_int(macros, "EIGEN_DEFAULT_ALIGN_BYTES"), macro_int(macros, "EIGEN_MALLOC_ALREADY_ALIGNED")))
    assert routes[0] == routes[1], "Compiled owners have different allocation routes"
    alignment, aligned = routes[0]
    assert aligned in {0, 1} and (alignment == 0 or alignment & (alignment - 1) == 0)
    expected = {(shape, "admitted", count) for shape in SHAPES for count in COUNTS}
    expected |= {(shape, "denied", count) for shape in SHAPES for count in COUNTS if count}
    actual = [(c["shape"], c["mode"], c["count"]) for c in cases if c["shape"] in SHAPES]
    assert len(actual) == len(set(actual)) and set(actual) == expected
    assert len(cases) == len(expected) + 3
    seed_case = next(c for c in cases if c["mode"] == "seed")
    seed = seed_case["impl"]
    assert seed > 0
    report = []
    for case in cases:
        events = case["events"]
        assert len(events) == case["event_count"] <= 256, "Dropped or overflowing observations"
        live, pending, completed, samples = {"c": {}, "new": {}}, [], [], []
        for event in events:
            assert event["phase"] in PHASES
            assert all(event[k] >= 0 for k in ("first", "second", "pointer", "previous", "root", "op"))
            if event["action"] == "sample":
                assert event["kind"] == "ledger" and not pending
                samples.append(event)
                continue
            if event["action"] == "begin":
                assert event["kind"] in ALLOCATIONS_C | ALLOCATIONS_NEW | DELETIONS | {"free"}
                pending.append(event)
                continue
            assert event["action"] == "end" and pending
            begin = pending.pop()
            assert all(begin[k] == event[k] for k in ("kind", "phase", "first", "second", "previous"))
            completed.append((begin, event))
            kind = event["kind"]
            channel = "c" if kind in ALLOCATIONS_C or kind == "free" else "new"
            if kind in ALLOCATIONS_C or kind in ALLOCATIONS_NEW:
                assert event["pointer"], "Actual original request failed"
                if kind == "realloc" and event["previous"]:
                    assert event["first"] > 0 and event["previous"] in live[channel]
                    del live[channel][event["previous"]]
                assert event["pointer"] not in live[channel]
                size = event["first"] * event["second"] if kind == "calloc" else event["first"]
                live[channel][event["pointer"]] = (size, event["second"], kind)
            else:
                assert kind == "free" or kind in DELETIONS
                assert begin["pointer"] == event["pointer"], "Free pointer changed between original-call boundaries"
                if event["pointer"]:
                    assert event["pointer"] in live[channel], "Free lacks observed original allocation"
                    size, allocation_alignment, allocation_kind = live[channel][event["pointer"]]
                    if channel == "new":
                        assert ("array" in kind) == ("array" in allocation_kind)
                        assert ("aligned" in kind) == ("aligned" in allocation_kind)
                        assert event["second"] == allocation_alignment
                        assert event["first"] == (size if "sized" in kind else 0)
                    del live[channel][event["pointer"]]
        assert not pending and not live["c"] and not live["new"]
        mode = case["mode"]
        if mode == "control":
            allocations = [(a["kind"], a["first"], a["second"]) for a, _ in completed
                           if a["kind"] in ALLOCATIONS_C | ALLOCATIONS_NEW]
            assert allocations == [("malloc", 37, 0), ("realloc", 53, 0),
                                   ("calloc", 3, 37), ("new", 19, 0),
                                   ("new-aligned", 19, 64)]
            assert all(e["root"] == e["op"] == 0 for e in events)
            assert all(e["phase"] == "calibration" for e in events)
        elif mode == "seed":
            allocations = [(a, b) for a, b in completed if a["kind"] in ALLOCATIONS_NEW | ALLOCATIONS_C]
            assert len(allocations) == 1
            begin, end = allocations[0]
            assert begin["kind"] == "new-aligned" and begin["first"] == seed
            assert begin["phase"] == "seed" and end["pointer"] % begin["second"] == 0
            assert all(a["phase"] == "seed-destroy" for a, _ in completed
                       if a["kind"] in DELETIONS | {"free"})
            assert case["root_before"] == case["root_after"] == 0
            assert all(e["root"] == seed and e["op"] == 0 for e in events)
        elif mode == "seed-denied":
            assert not completed and len(samples) == 1
            assert samples[0]["root"] == samples[0]["op"] == 0
        else:
            scalar_bytes, width = SHAPES[case["shape"]]
            assert (case["scalar_bytes"], case["width"]) == (scalar_bytes, width)
            payload = scalar_bytes * width * case["count"]
            expected_numeric = payload + (alignment if payload and alignment and not aligned else 0)
            assert case["numeric"] == expected_numeric
            assert (case["impl"] > 0) == (case["count"] > 0)
            assert case["qualified"] in {0, 1}
            assert case["root_before"] == case["op_before"] == case["root_after"] == case["op_after"] == seed
            if mode == "denied":
                assert not completed and len(samples) == 1
                assert samples[0]["root"] == samples[0]["op"] == seed
            else:
                charged = seed + case["impl"] + case["numeric"]
                assert [s["phase"] for s in samples] == ["admitted", "owner-freed", "lease-freed"]
                assert all(s["root"] == s["op"] == charged for s in samples[:2])
                assert samples[2]["root"] == samples[2]["op"] == seed
                assert all(e["root"] == e["op"] == charged for e in events if e["action"] != "sample")
                allocations = [(a, b) for a, b in completed if a["kind"] in ALLOCATIONS_NEW | ALLOCATIONS_C]
                if payload:
                    assert len(allocations) == 2, "Expected original PIMPL-new and Eigen-malloc only"
                    implementation = next(b for a, b in allocations if a["kind"] == "new")
                    numeric = next(b for a, b in allocations if a["kind"] == "malloc")
                    assert implementation["first"] == case["impl"] and numeric["first"] == case["numeric"]
                    assert case["data"] != 0
                    if alignment == 0 or aligned:
                        assert case["data"] == numeric["pointer"]
                    else:
                        assert 0 < case["data"] - numeric["pointer"] <= alignment
                    if alignment:
                        assert case["data"] % alignment == 0
                    assert case["data"] + payload <= numeric["pointer"] + numeric["first"]
                    frees = [a for a, _ in completed if a["kind"] in DELETIONS | {"free"}]
                    assert [(a["kind"], a["pointer"]) for a in frees] in (
                        [("free", numeric["pointer"]), ("delete", implementation["pointer"])],
                        [("free", numeric["pointer"]), ("delete-sized", implementation["pointer"])])
                    assert all(a["phase"] == "construct" for a, _ in allocations)
                    assert all(a["phase"] == "destroy" for a in frees)
                else:
                    assert not completed and case["data"] == 0
        report.append({k: v for k, v in case.items() if k != "events"})
    assert sum(c["mode"] == "control" for c in cases) == 1
    assert sum(c["mode"] == "seed-denied" for c in cases) == 1
    return {"profile": profile, "sources": sources, "compiled_alignment": alignment,
            "compiled_malloc_already_aligned": aligned, "cases": report,
            "original_request_plan_correspondence": True, "public_factory_admission_tested": False,
            "production_predicate_modified": False,
            "full_materialization_qualified": False, "observations_are_original_call_layers_not_RSS": True}

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("owner_profile", type=Path)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    result = verify(args.trace.read_text(encoding="utf-8"), json.loads(args.owner_profile.read_text(encoding="utf-8")))
    with args.report.open("x", encoding="utf-8") as output:
        output.write(json.dumps(result, indent=2) + "\n")
    print("Verified", len(result["cases"]), "original request/admission cases")
