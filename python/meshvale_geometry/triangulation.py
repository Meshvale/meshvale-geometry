# SPDX-License-Identifier: Apache-2.0
"""Synchronous exact polygon conversion with owned results and shared admission."""
from dataclasses import dataclass
import sys

from ._geometry import Cancellation, ExecutionContext, Mesh, _triangulate


def _integer(value, name, maximum):
    if type(value) is not int:
        raise TypeError(f"{name} must be a built-in integer")
    if value < 0:
        raise ValueError(f"{name} must be nonnegative")
    if value > maximum:
        raise OverflowError(f"{name} exceeds its native integer range")


@dataclass(frozen=True)
class TriangulationOptions:
    max_corners_per_face: int = 4096
    minimum_parallel_faces: int = 64

    def __post_init__(self):
        _integer(self.max_corners_per_face, "max_corners_per_face", 2**64 - 1)
        _integer(self.minimum_parallel_faces, "minimum_parallel_faces", 2 * sys.maxsize + 1)
        if not 3 <= self.max_corners_per_face <= 4096:
            raise ValueError("max_corners_per_face must be between 3 and 4096")


@dataclass(frozen=True)
class Diagnostic:
    code: str
    subject: str
    element: int | None


@dataclass(frozen=True)
class TriangulationResult:
    status: str
    candidate: Mesh | None
    diagnostics: tuple[Diagnostic, ...]
    face_sources: memoryview
    corner_sources: memoryview
    face_output_offsets: memoryview
    workers_used: int
    serial_reason: str
    peak_tracked_payload_bytes: int


def triangulate(mesh, *, options=None, execution=None, cancellation=None):
    """Convert an immutable Mesh; blocked/canceled calls return no partial result.

    Passing one ExecutionContext shares native worker and declared-payload caps
    across concurrent calls. Python result/buffer marshalling is outside those
    caps. Cancellation can be requested by another Python thread during the
    GIL-released native computation. None creates a fresh context for this call.
    """
    if options is None:
        options = TriangulationOptions()
    if type(options) is not TriangulationOptions:
        raise TypeError("options must be TriangulationOptions or None")
    if execution is None:
        execution = ExecutionContext()
    raw = _triangulate(mesh, max_corners_per_face=options.max_corners_per_face,
                      minimum_parallel_faces=options.minimum_parallel_faces,
                      execution=execution, cancellation=cancellation)
    return TriangulationResult(raw[0], raw[1], tuple(Diagnostic(*item) for item in raw[2]),
                               *raw[3:])
