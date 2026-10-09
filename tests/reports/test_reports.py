# SPDX-License-Identifier: Apache-2.0
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys
import unittest

from meshvale_reports import ReportError, coverage_state, exit_code, load, profile_outcome, schema_document, validate

ROOT = Path(__file__).resolve().parents[2]


def accepted():
    return json.loads((ROOT / "examples/reports/accepted.json").read_text())


def without_export(report=None):
    report = accepted() if report is None else report
    report["snapshots"] = [s for s in report["snapshots"] if s["role"] != "output"]
    report["maps"] = [m for m in report["maps"] if m["to"]["snapshot"] != "output"]
    report["coverage"] = [c for c in report["coverage"] if c["id"] != "reload"]
    report["profile"]["required_checks"] = ["candidate-storage"]
    report["publication"] = {"requested": False, "outcome": "not-requested", "snapshot": None,
                             "artifacts": [], "verified": False, "reason": None}
    return report


def inspection():
    report = without_export()
    report["operation"] = {"kind": "inspection", "name": "inspect", "version": "1", "options": {}, "targets": []}
    report["candidate"] = {"outcome": "not-attempted", "snapshot": None}
    report["snapshots"] = report["snapshots"][:1]
    report["maps"] = []; report["preservation"] = []
    for check in report["coverage"]:
        check["scope"]["snapshot"] = "input"
    return report


def rejected():
    report = inspection()
    report["operation"]["kind"] = "repair"
    report["operation"]["targets"] = [{"snapshot": "input", "mesh": "surface", "domain": "face", "index": 999}]
    report["candidate"]["outcome"] = "rejected"
    return report


def batch(reports, mode="continue", outcome="completed", unattempted=False):
    result = {"schema": "meshvale.batch/1", "tool": {"name": "example-batch", "version": "1"}, "mode": mode,
              "execution": {"outcome": outcome, "stage": "complete" if outcome == "completed" else "report",
                            "reason": None if outcome == "completed" else "Batch interrupted."},
              "jobs": [{"id": "job-"+str(i), "input": deepcopy(r["input"]), "outcome": "attempted", "report": r, "reason": None}
                       for i, r in enumerate(reports)], "exit_code": 0, "extensions": {}}
    if unattempted:
        result["jobs"].append({"id": "remaining", "input": {"id": "other", "sha256": None},
                               "outcome": "not-attempted", "report": None, "reason": "Scheduling stopped."})
    codes = [exit_code(r) for r in reports] + ([130 if outcome == "cancelled" else 2] if outcome != "completed" else [])
    result["exit_code"] = next((c for c in (130, 2, 1) if c in codes), 0)
    return result


class Reports(unittest.TestCase):
    def test_schema_and_declarative_module_do_not_load_native_geometry(self):
        from jsonschema import Draft202012Validator
        Draft202012Validator.check_schema(schema_document())
        self.assertNotIn("meshvale_geometry", sys.modules)
        original = schema_document(); original["$defs"].clear()
        self.assertTrue(schema_document()["$defs"])

    def test_accepted_publication_and_attribute_sets(self):
        report = accepted(); before = deepcopy(report)
        validate(report); self.assertEqual(exit_code(report), 0)
        self.assertEqual(report, before)
        self.assertEqual([c["feature"]["set_index"] for c in report["preservation"][:2]], [0, 1])
        self.assertEqual(load(json.dumps(report)), report)
        self.assertEqual(profile_outcome(report), "passed")

    def test_required_optional_failed_and_blocked_coverage(self):
        report = without_export()
        self.assertEqual(exit_code(report), 0)
        report["profile"]["required_checks"].append("intersection")
        self.assertEqual(profile_outcome(report), "incomplete")
        with self.assertRaises(ReportError): validate(report)
        report["profile"]["outcome"] = "incomplete"
        self.assertEqual(exit_code(report), 1)
        for status in ("skipped", "failed", "unsupported"):
            report["coverage"][0].update(status=status, finding=None, reason="Precondition unavailable.")
            validate(report)
        report["coverage"][0].update(status="performed", finding="failed", reason=None)
        self.assertEqual(profile_outcome(report), "failed")
        report["profile"]["outcome"] = "failed"; self.assertEqual(exit_code(report), 1)
        report["profile"]["required_checks"].append("absent")
        with self.assertRaises(ReportError): validate(report)

    def test_native_coverage_translation_does_not_guess_predicate_results(self):
        for finding in ("passed", "failed"):
            self.assertEqual(coverage_state("performed", finding=finding)["finding"], finding)
        with self.assertRaises(ReportError): coverage_state("performed")
        self.assertEqual(coverage_state("blocked", reason="Malformed storage.")["status"], "skipped")
        self.assertEqual(coverage_state("unsupported", reason="No implementation.")["status"], "unsupported")
        for status in ("blocked", "unsupported", "unknown"):
            with self.assertRaises(ReportError): coverage_state(status)

    def test_one_to_many_polygon_conversion_correspondence(self):
        report = accepted(); report["candidate"]["outcome"] = "unchanged"
        for snapshot in report["snapshots"]:
            snapshot["meshes"][0].update(vertex_count=4, face_count=1, corner_count=4)
        report["snapshots"][2]["meshes"][0].update(face_count=2, corner_count=6)
        for relation in report["maps"][:3]: relation.update(kind="identity", entries=None)
        report["maps"][4].update(kind="explicit", entries=[[0, 1]])
        report["maps"][5].update(kind="explicit", entries=[[0, 3], 1, [2, 4], 5])
        report["preservation"].append({"feature": {"kind": "geometry", "name": "polygon-loops", "domain": "face", "set_index": None},
            "from": {"snapshot": "candidate", "mesh": "surface", "subject": "polygons"},
            "to": {"snapshot": "output", "mesh": "surface", "subject": "triangles"},
            "outcome": "converted", "reason": "Declared quad triangulation.", "maps": ["export-faces", "export-corners"]})
        validate(report)
        for entry in ([[0, 0]], [[0, 2]], [[]]):
            invalid = deepcopy(report); invalid["maps"][4]["entries"] = entry
            with self.assertRaises(ReportError): validate(invalid)

    def test_rejected_partial_requests_and_unchanged_identity(self):
        self.assertEqual(exit_code(rejected()), 1)
        report = without_export(); report["candidate"]["outcome"] = "unchanged"
        report["snapshots"][1]["meshes"] = deepcopy(report["snapshots"][0]["meshes"])
        for relation in report["maps"]:
            relation.update(kind="identity", entries=None)
        self.assertEqual(exit_code(report), 0)
        report["maps"].pop()
        with self.assertRaises(ReportError): validate(report)
        self.assertEqual(exit_code(inspection()), 0)

    def test_execution_export_cancellation_and_report_write_failure(self):
        for status, code in (("failed", 2), ("cancelled", 130)):
            report = without_export()
            report["execution"] = {"outcome": status, "stage": "export", "reason": "Export interrupted."}
            report["publication"].update(requested=True, outcome=status, reason="Export interrupted.")
            self.assertEqual(exit_code(report), code)
            self.assertEqual(report["candidate"]["outcome"], "accepted")
        report = accepted()
        report["execution"] = {"outcome": "failed", "stage": "report", "reason": "Report write failed."}
        self.assertEqual(exit_code(report), 2)
        self.assertEqual(report["publication"]["outcome"], "published")
        report = inspection(); report["input"] = None; report["operation"] = None
        report["snapshots"] = []; report["coverage"] = []; report["profile"].update(required_checks=[], outcome="passed")
        report["execution"] = {"outcome": "failed", "stage": "import", "reason": "Invalid invocation or import."}
        self.assertEqual(exit_code(report), 2)

    def test_contradictory_success_and_orphaned_references(self):
        mutations = [lambda r: r["publication"].update(verified=False),
                     lambda r: r["candidate"].update(outcome="rejected"),
                     lambda r: r["publication"].update(requested=False),
                     lambda r: r["coverage"][0]["scope"].update(snapshot="stale"),
                     lambda r: r["coverage"][0]["diagnostics"].append("missing"),
                     lambda r: r["preservation"][0]["maps"].append("missing"),
                     lambda r: r["preservation"][0]["maps"].append("export-corners"),
                     lambda r: r["preservation"][0]["maps"].clear(),
                     lambda r: r["snapshots"].append(deepcopy(r["snapshots"][0])),
                     lambda r: r["maps"].clear()]
        for change in mutations:
            with self.subTest(change=change):
                report = accepted(); change(report)
                with self.assertRaises(ReportError): validate(report)
        report = without_export(); report["publication"].update(requested=True, outcome="not-attempted", reason="Missing output.")
        with self.assertRaises(ReportError): validate(report)

    def test_map_direction_cardinality_bounds_and_exact_integer_indices(self):
        mutations = [lambda r: r["maps"][1].update(entries=[0]),
                     lambda r: r["maps"][1].update(entries=[0, 1]),
                     lambda r: r["maps"][1].update(entries=[0, -1]),
                     lambda r: r["maps"][1].update(entries=[0, True]),
                     lambda r: r["maps"][1].update(entries=[0, 0.0]),
                     lambda r: r["maps"][0].update(kind="identity", entries=[0, 1, 2]),
                     lambda r: r["maps"][0]["to"].update(snapshot="input")]
        for change in mutations:
            with self.subTest(change=change):
                report = accepted(); change(report)
                with self.assertRaises(ReportError): validate(report)
        report = accepted(); report["maps"][1]["entries"] = [0, None]
        validate(report)  # deleted rows and many-to-one maps have explicit representations

    def test_schema_evolution_unknown_fields_namespaced_extensions_and_strict_json(self):
        for change in [lambda r: r.update(schema="meshvale.report/2"), lambda r: r.update(extra=True),
                       lambda r: r.pop("coverage"), lambda r: r.update(extensions={"unnamespaced": {}}),
                       lambda r: r["snapshots"][0]["meshes"][0].update(vertex_count=2**64),
                       lambda r: r["operation"]["options"].update(tolerance=float("nan")),
                       lambda r: r["operation"]["options"].update(values=(1, 2))]:
            report = accepted(); change(report)
            with self.assertRaises(ReportError): validate(report)
        report = accepted(); report["extensions"]["vendor.future"] = {"unknown": [1, True, None]}; validate(report)
        for text in ('{"schema":"x","schema":"y"}', '{"a":NaN}', '{"a":1e999}'):
            with self.assertRaises(ReportError): load(text)
        with self.assertRaises(ReportError): profile_outcome({})
        report = accepted(); report["extensions"]["vendor.cycle"] = report
        with self.assertRaises(ReportError): validate(report)

    def test_batch_isolation_fail_fast_unattempted_and_precedence(self):
        good, policy = accepted(), rejected()
        failed = without_export(); failed["execution"] = {"outcome": "failed", "stage": "verify", "reason": "Verification failed."}
        cancelled = without_export(); cancelled["execution"] = {"outcome": "cancelled", "stage": "verify", "reason": "Cancelled."}
        for rows, code in (([good], 0), ([good, policy], 1), ([policy, failed], 2), ([failed, cancelled], 130)):
            value = batch(rows); self.assertEqual(exit_code(value), code)
        value = batch([policy], mode="fail-fast", unattempted=True); self.assertEqual(exit_code(value), 1)
        value["mode"] = "continue"
        with self.assertRaises(ReportError): validate(value)
        value = batch([], outcome="cancelled", unattempted=True); self.assertEqual(exit_code(value), 130)
        value = batch([good], outcome="failed"); self.assertEqual(exit_code(value), 2)
        value["exit_code"] = 0
        with self.assertRaises(ReportError): validate(value)
        value = batch([good]); value["jobs"][0]["input"]["id"] = "wrong"
        with self.assertRaises(ReportError): validate(value)


if __name__ == "__main__":
    unittest.main()
