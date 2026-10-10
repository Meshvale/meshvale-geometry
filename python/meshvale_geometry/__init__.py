# SPDX-License-Identifier: Apache-2.0
"""Owned polygon snapshots and explicit buffer exchange."""
from ._geometry import Mesh
from ._version import __version__
from .triangulation import (Cancellation, Diagnostic, ExecutionContext,
                            TriangulationOptions, TriangulationResult, triangulate)

__all__ = ["Mesh", "__version__", "Cancellation", "Diagnostic", "ExecutionContext",
           "TriangulationOptions", "TriangulationResult", "triangulate"]
