# SPDX-License-Identifier: Apache-2.0
"""Installed Python conversion coverage, ownership and execution acceptance."""
from array import array
import copy
from dataclasses import FrozenInstanceError
import gc
from pathlib import Path
import sys
import threading
import time
import unittest

from meshvale_geometry import (Cancellation, ExecutionContext, Mesh,
                               TriangulationOptions, triangulate)

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from triangulation_fixtures import cases, mesh as fixture_mesh
from triangulation_fraction_oracle import verify

KINDS = ["float32", "float64", "int32", "uint8", "uint16", "uint32", "uint64"]
FORMATS = ["f", "d", "i", "B", "H", "I", "Q"]
WIDTHS = [4, 8, 4, 1, 2, 4, 8]
DOMAINS = ["vertex", "face", "corner"]


def buffer(kind, values):
    data = b"".join(value.to_bytes(WIDTHS[kind], sys.byteorder) for value in values)
    return memoryview(data).cast(FORMATS[kind])


def record(fixture):
    return {"schema": "meshvale.mesh/1",
            "positions": buffer(1, [v for p in fixture["positions"] for v in p]),
            "face_offsets": array("Q", fixture["face_offsets"]),
            "corner_vertices": array("Q", fixture["corner_vertices"]),
            "attributes": [{"domain": DOMAINS[a["domain"]], "name": a["name"],
                            "semantic": a["semantic"], "set_index": a["set"],
                            "components": a["components"], "scalar_type": KINDS[a["type"]],
                            "values": buffer(a["type"], a["values"]),
                            "offsets": None if a["offsets"] is None else array("Q", a["offsets"]),
                            "present": None if a["present"] is None else array("B", a["present"]),
                            "metadata": dict(a["metadata"])} for a in fixture["attributes"]]}


def bits(view, kind):
    data, width = view.tobytes(), WIDTHS[kind]
    return [int.from_bytes(data[i:i + width], sys.byteorder)
            for i in range(0, len(data), width)]


def raw(snapshot):
    r = snapshot.to_record()
    positions = bits(r["positions"], 1)
    return {"positions": [positions[i:i + 3] for i in range(0, len(positions), 3)],
            "face_offsets": list(r["face_offsets"]), "corner_vertices": list(r["corner_vertices"]),
            "attributes": [{"domain": DOMAINS.index(a["domain"]), "name": a["name"],
                            "semantic": a["semantic"], "set": a["set_index"],
                            "components": a["components"], "type": KINDS.index(a["scalar_type"]),
                            "values": bits(a["values"], KINDS.index(a["scalar_type"])),
                            "offsets": None if a["offsets"] is None else list(a["offsets"]),
                            "present": None if a["present"] is None else list(a["present"]),
                            "metadata": dict(a["metadata"])} for a in r["attributes"]]}


def outcome(result, source):
    return {"mesh": raw(result.candidate), "face_sources": list(result.face_sources),
            "corner_sources": list(result.corner_sources),
            "face_output_offsets": list(result.face_output_offsets),
            "source": raw(source), "source_unchanged": True}


def busy_mesh():
    return Mesh.from_record(record(fixture_mesh(
        [(i, i * i, 0) for i in range(256)], [list(range(256))] * 128)))


def start_conversion(source, context, cancellation):
    results, errors = [], []
    def run():
        try:
            results.append(triangulate(source, options=TriangulationOptions(minimum_parallel_faces=0),
                                       execution=context, cancellation=cancellation))
        except BaseException as error:
            errors.append(error)
    thread = threading.Thread(target=run)
    thread.start()
    return thread, results, errors


def wait_active(context, thread, minimum_payload=0):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if context.active_workers == 4 and context.active_tracked_payload_bytes > minimum_payload:
            return
        if not thread.is_alive():
            break
        time.sleep(0.001)
    raise AssertionError("another Python thread did not observe the active native computation")


class TriangulationTests(unittest.TestCase):
    def assert_empty(self, result, status):
        self.assertEqual(result.status, status)
        self.assertIsNone(result.candidate)
        for view in [result.face_sources, result.corner_sources, result.face_output_offsets]:
            self.assertEqual((view.format, view.itemsize, len(view), view.readonly), ("Q", 8, 0, True))

    def test_fraction_geometry_correspondence_and_all_scalar_bits(self):
        count = 0
        for fixture in cases():
            source = Mesh.from_record(record(fixture))
            results = []
            for workers in [1, 4]:
                context = ExecutionContext(worker_budget=workers)
                result = triangulate(source, options=TriangulationOptions(minimum_parallel_faces=0),
                                     execution=context)
                self.assertEqual(result.status, "accepted")
                self.assertIsInstance(result.candidate, Mesh)
                result_raw = outcome(result, source)
                verify(fixture, result_raw)
                self.assertEqual(raw(source), fixture)
                for view in [result.face_sources, result.corner_sources, result.face_output_offsets]:
                    self.assertEqual((view.format, view.itemsize, view.readonly), ("Q", 8, True))
                self.assertEqual((context.active_workers, context.active_tracked_payload_bytes), (0, 0))
                self.assertLessEqual(context.peak_workers, context.worker_budget)
                self.assertLessEqual(result.peak_tracked_payload_bytes, context.tracked_payload_budget_bytes)
                if workers == 4 and len(fixture["face_offsets"]) > 2:
                    self.assertEqual(result.workers_used, min(4, len(fixture["face_offsets"]) - 1))
                results.append(result_raw)
                count += 1
            self.assertEqual(results[0], results[1])
        self.assertEqual(count, 94)
        print("Installed Python Fraction coverage/winding/maps/scalar-bit proof: 94 calls")

    def test_blocked_profile_no_partial_and_source_unchanged(self):
        for points, code in [
            ([(0, 0, 0), (1, 1, 1), (2, 2, 2)], "conversion.degenerate_face"),
            ([(0, 0, 0), (2, 2, 0), (0, 2, 0), (2, 0, 0)], "conversion.self_intersection"),
            ([(0, 0, 0), (1, 0, 0), (1, 1, 5e-324), (0, 1, 0)], "conversion.nonplanar_face"),
            ([(0, 0, 0), (2, 0, 0), (2, 2, 0), (0, 0, 0)], "conversion.repeated_position")]:
            fixture = fixture_mesh(points)
            source = Mesh.from_record(record(fixture))
            result = triangulate(source)
            self.assert_empty(result, "blocked")
            self.assertEqual(result.diagnostics[0].code, code)
            self.assertEqual(raw(source), fixture)
        source = Mesh.from_record(record(cases()[8]))
        result = triangulate(source, options=TriangulationOptions(max_corners_per_face=3))
        self.assert_empty(result, "blocked")
        self.assertEqual(result.diagnostics[0].code, "conversion.face_corner_limit")
        source = Mesh.from_record({**record(fixture_mesh([(0, 0, 0)] * 3)),
                                   "corner_vertices": array("Q", [0, 1, 99])})
        self.assert_empty(triangulate(source), "blocked")
        fixture = copy.deepcopy(cases()[-4])
        fixture["corner_vertices"][-5:] = [0, 0, 0, 0, 0]
        source = Mesh.from_record(record(fixture))
        result = triangulate(source, options=TriangulationOptions(minimum_parallel_faces=0),
                             execution=ExecutionContext(worker_budget=4))
        self.assert_empty(result, "blocked")
        self.assertEqual(raw(source), fixture)
        diagnostic = result.diagnostics[0]
        del source, result
        gc.collect()
        self.assertEqual((diagnostic.code, diagnostic.subject, diagnostic.element),
                         ("conversion.repeated_position", "face", 2))

    def test_strict_configuration_and_frozen_values(self):
        for field in ["max_corners_per_face", "minimum_parallel_faces"]:
            for value in [True, False, 1.5, "4", None]:
                with self.assertRaises(TypeError):
                    TriangulationOptions(**{field: value})
            with self.assertRaises(ValueError):
                TriangulationOptions(**{field: -1})
            with self.assertRaises(OverflowError):
                TriangulationOptions(**{field: 2**64})
        for value in [0, 1, 2, 4097]:
            with self.assertRaises(ValueError):
                TriangulationOptions(max_corners_per_face=value)
        for field in ["worker_budget", "minimum_parallel_vertices", "tracked_payload_budget_bytes"]:
            for value in [True, False, 1.0, "1", None]:
                with self.assertRaises(TypeError):
                    ExecutionContext(**{field: value})
            with self.assertRaises(ValueError):
                ExecutionContext(**{field: -1})
            with self.assertRaises(OverflowError):
                ExecutionContext(**{field: 2**64})
        class Integer(int):
            pass
        with self.assertRaises(TypeError):
            TriangulationOptions(minimum_parallel_faces=Integer(0))
        with self.assertRaises(TypeError):
            ExecutionContext(worker_budget=Integer(1))
        with self.assertRaises(FrozenInstanceError):
            TriangulationOptions().minimum_parallel_faces = 0
        result = triangulate(Mesh())
        with self.assertRaises(FrozenInstanceError):
            result.status = "blocked"
        blocked = triangulate(Mesh.from_record(record(fixture_mesh([(0, 0, 0)] * 3))))
        with self.assertRaises(FrozenInstanceError):
            blocked.diagnostics[0].code = "other"
        with self.assertRaises(TypeError):
            triangulate({})
        for name, value in [("options", {}), ("execution", True), ("cancellation", True)]:
            with self.assertRaises(TypeError):
                triangulate(Mesh(), **{name: value})
        # Recheck native inputs even if Python's frozen guard is deliberately bypassed.
        options = TriangulationOptions()
        object.__setattr__(options, "minimum_parallel_faces", True)
        with self.assertRaises(TypeError):
            triangulate(Mesh(), options=options)
        context = ExecutionContext(worker_budget=1, tracked_payload_budget_bytes=0)
        with self.assertRaises(AttributeError):
            context.worker_budget = 4
        self.assert_empty(triangulate(Mesh(), execution=context), "blocked")

    def test_result_lifetimes_and_pre_cancellation(self):
        fixture = cases()[-3]
        source, context, cancellation = Mesh.from_record(record(fixture)), ExecutionContext(worker_budget=4), Cancellation()
        result = triangulate(source, options=TriangulationOptions(minimum_parallel_faces=0),
                             execution=context, cancellation=cancellation)
        candidate, maps = result.candidate, (result.face_sources, result.corner_sources, result.face_output_offsets)
        self.assertFalse(cancellation.stop_requested)
        del source, context, cancellation, result
        gc.collect()
        source = Mesh.from_record(record(fixture))
        verify(fixture, {"mesh": raw(candidate), "face_sources": list(maps[0]),
                         "corner_sources": list(maps[1]), "face_output_offsets": list(maps[2]),
                         "source": raw(source), "source_unchanged": True})
        with self.assertRaises(TypeError):
            maps[0][0] = 99
        before = raw(candidate)
        r = candidate.to_record(); r["attributes"].clear()
        self.assertEqual(raw(candidate), before)
        cancellation = Cancellation()
        self.assertTrue(cancellation.request_stop())
        self.assertFalse(cancellation.request_stop())
        self.assert_empty(triangulate(source, cancellation=cancellation), "canceled")

    def test_shared_workers_gil_release_and_midcall_cancellation(self):
        context, cancellation = ExecutionContext(worker_budget=4), Cancellation()
        shared = copy.copy(context)
        source = busy_mesh()
        thread, results, errors = start_conversion(source, context, cancellation)
        try:
            wait_active(shared, thread)
            result = triangulate(Mesh.from_record(record(cases()[-2])),
                                 options=TriangulationOptions(minimum_parallel_faces=0), execution=shared)
            self.assertEqual(result.status, "accepted")
            self.assertEqual(result.workers_used, 0)
            self.assertIn("shared worker budget unavailable", result.serial_reason)
        finally:
            cancellation.request_stop()
            thread.join(15)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        self.assert_empty(results[0], "canceled")
        self.assertEqual(results[0].workers_used, 4)
        self.assertEqual((shared.active_workers, shared.active_tracked_payload_bytes), (0, 0))
        self.assertEqual(shared.peak_workers, 4)

    def test_shared_payload_admission(self):
        positions = array("d", [0, 0, 0, 1, 0, 0, 0, 1, 0])
        positions.extend([0.0] * (3 * (80000 - 3)))
        probe = Mesh.from_record({"schema": "meshvale.mesh/1", "positions": positions,
                                  "face_offsets": array("Q", [0, 3]),
                                  "corner_vertices": array("Q", [0, 1, 2]), "attributes": []})
        peak = triangulate(probe, execution=ExecutionContext(worker_budget=1)).peak_tracked_payload_bytes
        context = ExecutionContext(worker_budget=4, tracked_payload_budget_bytes=peak)
        cancellation = Cancellation()
        thread, results, errors = start_conversion(busy_mesh(), context, cancellation)
        try:
            wait_active(context, thread, peak // 2)
            blocked = triangulate(probe, execution=context)
            self.assert_empty(blocked, "blocked")
            self.assertEqual(blocked.diagnostics[0].code, "conversion.payload_budget")
        finally:
            cancellation.request_stop()
            thread.join(15)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        self.assert_empty(results[0], "canceled")
        self.assertLessEqual(context.peak_tracked_payload_bytes, peak)
        self.assertEqual(context.active_tracked_payload_bytes, 0)
        self.assertEqual(triangulate(probe, execution=context).status, "accepted")


if __name__ == "__main__":
    unittest.main()
