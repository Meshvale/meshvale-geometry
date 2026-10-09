# SPDX-License-Identifier: Apache-2.0
"""Run against an installed meshvale-geometry wheel, without NumPy."""
from array import array
from meshvale_geometry import Mesh

mesh = Mesh.from_record({
    "schema": "meshvale.mesh/1",
    "positions": array("d", [0,0,0, 1,0,0, 1,1,0, 0,1,0]),
    "face_offsets": array("Q", [0,4]),
    "corner_vertices": array("Q", [0,1,2,3]),
    "attributes": [],
})
inspection = mesh.inspect_topology()
assert mesh.face_count == 1 and mesh.corner_count == 4
assert inspection["diagnostics"] == []
assert inspection["topology"]["boundaries"][0]["kind"] == "cycle"
print("Quad preserved; boundary cycle inspected; geometric/solid coverage remains explicit.")
