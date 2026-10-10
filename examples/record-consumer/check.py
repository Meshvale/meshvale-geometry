# SPDX-License-Identifier: Apache-2.0
"""Run separately in both import orders, using only installed extensions."""
from array import array
import gc
import sys

if sys.argv[1:] == ["consumer-first"]:
    import meshvale_record_consumer as consumer
    from meshvale_geometry import Mesh
else:
    from meshvale_geometry import Mesh
    import meshvale_record_consumer as consumer

indices = consumer.write_indices()
gc.collect()
assert indices.readonly and list(indices) == [0, 2**53 + 1, 2**64 - 1]
empty_indices = consumer.empty_indices()
assert empty_indices.readonly and len(empty_indices) == 0

source = {"schema":"meshvale.mesh/1", "positions":array("d",[0,0,0, 1,0,0, 1,1,0, 0,1,0]),
          "face_offsets":array("Q",[0,4]), "corner_vertices":array("Q",[0,1,2,3]), "attributes":[]}
for kind,code in [("float32","f"),("float64","d"),("int32","i"),("uint8","B"),
                  ("uint16","H"),("uint32","I"),("uint64","Q")]:
    source["attributes"].append({"domain":"corner","name":kind,"semantic":"custom","set_index":None,
        "components":1,"scalar_type":kind,"values":array(code,[0,1,2,3]),
        "offsets":None,"present":array("B",[1,1,0,1]),"metadata":{"origin":"author"}})
source["attributes"][0].update({"offsets":array("Q",[0,2,2,3,4]),"semantic":"joint_weight"})
mesh = Mesh.from_record(source)
record = mesh.to_record()
returned = consumer.inspect_record(record)
assert returned["face_count"] == 1 and returned["corner_count"] == 4
assert returned["edge_count"] == 4 and returned["storage_issues"] == 0
assert not hasattr(consumer,"Mesh")
del mesh,source,record
gc.collect()
restored = Mesh.from_record(returned["record"])
assert restored.corner_count == 4 and restored.inspect_storage() == []
for row in returned["record"]["attributes"]:
    assert row["values"].readonly and list(row["values"]) == [0,1,2,3]
assert list(returned["record"]["attributes"][0]["offsets"]) == [0,2,2,3,4]
bad = restored.to_record(); bad["corner_vertices"] = array("Q",[0,1,2,99])
assert consumer.inspect_record(bad)["edge_count"] is None
bad["positions"] = array("f",[0]*12)
try:
    consumer.inspect_record(bad)
except ValueError:
    pass
else:
    raise AssertionError("incompatible scalar buffer was silently accepted")
print("Independent installed record consumer passed:", sys.argv[1:])
