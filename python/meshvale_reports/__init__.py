# SPDX-License-Identifier: Apache-2.0
"""Declarative reports and reference semantics; no native mesh import required."""
from importlib.resources import files
import json
import math

__all__ = ["ReportError", "schema_document", "load", "validate", "profile_outcome", "exit_code", "coverage_state"]


class ReportError(ValueError):
    """A report contradicts the versioned structure or semantic contract."""


def schema_document():
    """Return a fresh schema dictionary without loading native or validator code."""
    return json.loads(files(__package__).joinpath("report-v1.schema.json").read_text(encoding="utf-8"))


def load(text):
    """Read strict JSON and validate it; duplicate object keys are refused."""
    def object_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ReportError("duplicate JSON key: " + key)
            result[key] = value
        return result
    def constant(value):
        raise ReportError("nonfinite JSON number: " + value)
    try:
        result = json.loads(text, object_pairs_hook=object_pairs, parse_constant=constant)
    except (ValueError, TypeError) as error:
        raise ReportError(str(error)) from error
    validate(result)
    return result


def _require(condition, message):
    if not condition:
        raise ReportError(message)


def _json(value, active=None):
    if value is None or type(value) in (str, bool, int):
        return
    if type(value) is float and math.isfinite(value):
        return
    if type(value) is list or type(value) is dict and all(type(key) is str for key in value):
        active = set() if active is None else active
        _require(id(value) not in active, "report contains a cyclic container")
        active.add(id(value))
        try:
            for item in value if type(value) is list else value.values():
                _json(item, active)
        finally:
            active.remove(id(value))
        return
    raise ReportError("report contains non-JSON values or nonfinite numbers")


def _unique(rows, label):
    result = {}
    for row in rows:
        _require(row["id"] not in result, "duplicate " + label + " identity: " + row["id"])
        result[row["id"]] = row
    return result


def _profile(report, checks):
    required = report["profile"]["required_checks"]
    _require(all(name in checks for name in required), "profile references missing coverage")
    selected = [checks[name] for name in required]
    if any(row["status"] == "performed" and row["finding"] == "failed" for row in selected):
        return "failed"
    if any(row["status"] != "performed" for row in selected):
        return "incomplete"
    return "passed"


def _execution(value):
    if value["outcome"] == "completed":
        _require(value["stage"] == "complete", "completed execution requires complete stage")
    else:
        _require(value["stage"] != "complete" and value["reason"] is not None,
                 "failed/cancelled execution requires stage and reason")


def coverage_state(native_status, *, finding=None, reason=None):
    """Translate native coverage; performed predicates require an explicit finding."""
    if native_status == "performed":
        _require(finding in ("passed", "failed"), "performed coverage needs the predicate's actual finding")
        _require(reason is None or type(reason) is str and bool(reason), "invalid coverage reason")
        return {"status": "performed", "finding": finding, "reason": reason}
    _require(native_status in ("blocked", "unsupported"), "unknown native coverage status")
    _require(finding is None and type(reason) is str and bool(reason), "unperformed native coverage needs reason and no finding")
    return {"status": "skipped" if native_status == "blocked" else "unsupported", "finding": None, "reason": reason}


def _report(report):
    execution, candidate, publication = (report[k] for k in ("execution", "candidate", "publication"))
    _execution(execution)
    snapshots = _unique(report["snapshots"], "snapshot")
    roles = [s["role"] for s in snapshots.values()]
    _require(len(roles) == len(set(roles)), "one snapshot per role is required")
    meshes = {s["id"]: _unique(s["meshes"], "mesh") for s in snapshots.values()}
    def mesh(ref):
        _require(ref["snapshot"] in meshes, "missing snapshot reference")
        _require(ref["mesh"] in meshes[ref["snapshot"]], "missing mesh reference")
        return meshes[ref["snapshot"]][ref["mesh"]]
    def scope(ref):
        if ref["snapshot"] is None:
            _require(ref["mesh"] is None, "mesh scope requires a snapshot")
        elif ref["mesh"] is None:
            _require(ref["snapshot"] in snapshots, "missing snapshot scope")
        else:
            mesh(ref)
    for row in snapshots.values():
        for item in row["meshes"]:
            _require(all(type(item[k]) is int for k in ("vertex_count", "face_count", "corner_count")),
                     "mesh counts require canonical integer values")
    if report["input"] is None or report["operation"] is None:
        _require(execution["outcome"] == "failed" and execution["stage"] == "import" and
                 candidate["outcome"] == "not-attempted", "absent invocation/input requires import failure")
    if report["input"] is None:
        _require(not snapshots, "absent input cannot own snapshots")
    if report["operation"]:
        if report["operation"]["kind"] == "inspection":
            _require(candidate["outcome"] == "not-attempted" and not publication["requested"],
                     "inspection does not attempt edits or export")
        for target in report["operation"]["targets"]:
            mesh(target)
            _require(snapshots[target["snapshot"]]["role"] == "input", "targets require input snapshot")
            _require(type(target["index"]) is int, "target indices require canonical integer values")
            # An out-of-range requested target is representable in a rejected operation report.
    cid = candidate["snapshot"]
    if cid is not None:
        _require(cid in snapshots and snapshots[cid]["role"] == "candidate", "invalid candidate snapshot")
    if candidate["outcome"] in ("accepted", "unchanged"):
        _require(cid is not None and "input" in roles, "accepted/unchanged requires input and candidate snapshots")
        _require(execution["stage"] not in ("import", "inspect", "repair"), "candidate cannot precede repair")
    if candidate["outcome"] == "not-attempted":
        _require(cid is None and "candidate" not in roles, "unattempted candidate cannot have a snapshot")
    if "candidate" in roles:
        _require(cid is not None, "orphaned candidate snapshot")

    diagnostics = _unique(report["diagnostics"], "diagnostic")
    for row in diagnostics.values():
        scope(row["scope"])
        if row["element"] is not None:
            item = mesh(row["scope"])
            index, domain = row["element"]["index"], row["element"]["domain"]
            _require(type(index) is int and index < item[domain + "_count"], "diagnostic element is out of range")
    checks = _unique(report["coverage"], "coverage")
    for row in checks.values():
        scope(row["scope"])
        _require(all(name in diagnostics for name in row["diagnostics"]), "coverage references missing diagnostic")
        if row["status"] == "performed":
            _require(row["finding"] is not None, "performed coverage requires a finding")
        else:
            _require(row["finding"] is None and row["reason"] is not None, "unperformed coverage requires reason and no finding")
    _require(report["profile"]["outcome"] == _profile(report, checks), "profile outcome contradicts required coverage")

    maps = _unique(report["maps"], "map")
    pairs = set()
    for row in maps.values():
        source, destination = mesh(row["from"]), mesh(row["to"])
        relation = (row["from"]["snapshot"], row["from"]["mesh"], row["to"]["snapshot"], row["to"]["mesh"], row["domain"])
        _require(relation not in pairs, "duplicate correspondence relation")
        pairs.add(relation)
        order = {"input": 0, "candidate": 1, "output": 2}
        _require(order[snapshots[row["from"]["snapshot"]]["role"]] < order[snapshots[row["to"]["snapshot"]]["role"]],
                 "correspondence must point to a later snapshot role")
        before, after = (m[row["domain"] + "_count"] for m in (source, destination))
        if row["kind"] == "identity":
            _require(row["entries"] is None and before == after, "identity map requires equal counts and no entries")
        else:
            _require(row["entries"] is not None and len(row["entries"]) == before, "explicit map length differs from source count")
            for entry in row["entries"]:
                destinations = entry if type(entry) is list else [entry]
                _require(all(i is None or type(i) is int and i < after for i in destinations), "explicit map destination out of range")
    for row in report["preservation"]:
        scope(row["from"]); scope(row["to"])
        _require(all(name in maps for name in row["maps"]), "preservation references missing map")
        feature = row["feature"]
        _require(feature["kind"] != "attribute" or feature["domain"] is not None, "attribute feature requires a domain")
        _require(feature["kind"] == "attribute" or feature["set_index"] is None, "only attributes have set indices")
        if feature["set_index"] is not None:
            _require(type(feature["set_index"]) is int, "set index requires canonical integer value")
        if row["outcome"] != "preserved":
            _require(row["reason"] is not None, "conversion/unsupported/unverified claim requires a reason")
        if row["maps"]:
            _require(row["from"]["mesh"] is not None and row["to"]["mesh"] is not None, "mesh maps require mesh-scoped claims")
            for name in row["maps"]:
                relation = maps[name]
                for side in ("from", "to"):
                    _require(relation[side] == {k: row[side][k] for k in ("snapshot", "mesh")}, "claim/map scopes differ")
        if feature["kind"] in ("geometry", "attribute") and row["outcome"] in ("preserved", "converted"):
            _require(row["from"]["mesh"] is not None and row["to"]["mesh"] is not None, "mesh preservation requires mesh scopes")
            if row["from"] != row["to"]:
                _require(bool(row["maps"]), "preservation/conversion between meshes needs correspondence")
            if feature["domain"] is not None and row["maps"]:
                _require(any(maps[name]["domain"] == feature["domain"] for name in row["maps"]), "attribute/geometry domain lacks correspondence")

    if candidate["outcome"] in ("accepted", "unchanged"):
        input_snapshot = next(s for s in snapshots.values() if s["role"] == "input")
        for source in input_snapshot["meshes"]:
            relations = [m for m in maps.values() if m["from"] == {"snapshot":input_snapshot["id"], "mesh":source["id"]}
                         and m["to"]["snapshot"] == cid]
            _require({m["domain"] for m in relations} == {"vertex", "face", "corner"},
                     "accepted/unchanged requires every input mesh domain's correspondence")
            if candidate["outcome"] == "unchanged":
                _require(all(m["kind"] == "identity" for m in relations), "unchanged requires identity correspondence")

    artifacts = _unique(publication["artifacts"], "artifact")
    published = publication["outcome"] == "published"
    _require(publication["requested"] == (publication["outcome"] != "not-requested"), "publication request/outcome mismatch")
    if published:
        pid = publication["snapshot"]
        _require(pid in snapshots and snapshots[pid]["role"] == "output", "published output requires output snapshot")
        _require(publication["verified"] and bool(artifacts), "published output requires verified artifacts")
        _require(candidate["outcome"] in ("accepted", "unchanged") and report["profile"]["outcome"] == "passed",
                 "publication requires accepted/unchanged candidate and passed profile")
        _require(execution["outcome"] == "completed" or execution["stage"] == "report", "post-publication failure must be report stage")
    else:
        _require(publication["snapshot"] is None and not artifacts and not publication["verified"] and "output" not in roles,
                 "unpublished result cannot claim finalized output")
        if publication["outcome"] != "not-requested":
            _require(publication["reason"] is not None, "unpublished request requires a reason")
    for status in ("failed", "cancelled"):
        if publication["outcome"] == status:
            _require(execution["outcome"] == status and execution["stage"] in ("export", "report"), "publication failure/cancellation contradicts execution")
    if execution["stage"] == "export":
        _require(publication["requested"], "export stage requires a publication request")
    if execution["outcome"] == "completed" and candidate["outcome"] in ("accepted", "unchanged") and report["profile"]["outcome"] == "passed":
        _require(not publication["requested"] or published, "completed successful workflow has an unfinished requested export")


def _exit(report):
    if report["execution"]["outcome"] == "cancelled":
        return 130
    if report["execution"]["outcome"] == "failed":
        return 2
    unattempted_edit = report["candidate"]["outcome"] == "not-attempted" and report["operation"]["kind"] != "inspection"
    if report["candidate"]["outcome"] == "rejected" or unattempted_edit or report["profile"]["outcome"] != "passed":
        return 1
    return 0


def _batch(batch):
    _execution(batch["execution"])
    _unique(batch["jobs"], "job")
    codes = []
    unattempted = False
    for job in batch["jobs"]:
        if job["outcome"] == "attempted":
            _require(job["report"] is not None and job["reason"] is None, "attempted job requires its report")
            _report(job["report"])
            _require(job["report"]["input"] == job["input"], "job input differs from its report")
            codes.append(_exit(job["report"]))
        else:
            unattempted = True
            _require(job["report"] is None and job["reason"] is not None, "unattempted job requires reason and no report")
    if unattempted and batch["execution"]["outcome"] == "completed":
        _require(batch["mode"] == "fail-fast" and any(codes), "completed batch cannot silently omit jobs")
    if batch["execution"]["outcome"] != "completed":
        codes.append(130 if batch["execution"]["outcome"] == "cancelled" else 2)
    expected = next((code for code in (130, 2, 1) if code in codes), 0)
    _require(batch["exit_code"] == expected, "batch exit code contradicts outcome precedence")


def _structure(value):
    _json(value)
    from jsonschema import Draft202012Validator
    errors = list(Draft202012Validator(schema_document()).iter_errors(value))
    if errors:
        raise ReportError(errors[0].message)


def validate(value):
    """Validate strict JSON shape and reference semantics; return None on success."""
    _structure(value)
    if value["schema"] == "meshvale.report/1":
        _report(value)
    else:
        _batch(value)


def profile_outcome(report):
    """Compute required coverage without trusting a producer's declared outcome."""
    _structure(report)
    _require(report["schema"] == "meshvale.report/1", "profile_outcome requires a single report")
    # Validate a detached copy with the recomputed outcome, preserving caller data.
    copy = json.loads(json.dumps(report, allow_nan=False))
    copy["profile"]["outcome"] = _profile(copy, _unique(copy["coverage"], "coverage"))
    validate(copy)
    return copy["profile"]["outcome"]


def exit_code(value):
    """Compute single-asset or batch precedence after validating its declarations."""
    validate(value)
    return value["exit_code"] if value["schema"] == "meshvale.batch/1" else _exit(value)
