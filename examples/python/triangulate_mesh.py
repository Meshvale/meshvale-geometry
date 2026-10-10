# SPDX-License-Identifier: Apache-2.0
"""Triangulate an authored concave face while retaining its UV corner rows."""
from array import array
from meshvale_geometry import ExecutionContext, Mesh, triangulate

source = Mesh.from_record({
    "schema": "meshvale.mesh/1",
    "positions": array("d", [0, 0, 0, 4, 0, 0, 4, 4, 0, 2, 1, 0, 0, 4, 0]),
    "face_offsets": array("Q", [0, 5]),
    "corner_vertices": array("Q", [0, 1, 2, 3, 4]),
    "attributes": [{"domain": "corner", "name": "uv1", "semantic": "TEXCOORD",
                    "set_index": 1, "components": 2, "scalar_type": "float32",
                    "values": array("f", [0, 0, 1, 0, 1, 1, .5, .25, 0, 1]),
                    "offsets": None, "present": None, "metadata": {}}],
})
result = triangulate(source, execution=ExecutionContext(worker_budget=2))
assert result.status == "accepted" and result.candidate.face_count == 3
assert source.face_count == 1 and source.corner_count == 5
assert list(result.face_output_offsets) == [0, 3]
source_uv = source.to_record()["attributes"][0]["values"]
output_uv = result.candidate.to_record()["attributes"][0]["values"]
assert list(output_uv) == [source_uv[2 * corner + axis]
                          for corner in result.corner_sources for axis in range(2)]
print("Three triangles retain the authored concave boundary and UV corner correspondence.")
