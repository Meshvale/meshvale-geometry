# SPDX-License-Identifier: Apache-2.0
from array import array
import gc
import importlib.metadata
import struct
import unittest

from meshvale_geometry import Mesh, __version__


def triangle():
    return {"schema": "meshvale.mesh/1", "positions": array("d", [0,0,0, 1,0,0, 0,1,0]),
            "face_offsets": array("Q", [0,3]), "corner_vertices": array("Q", [0,1,2]), "attributes": []}


def attribute(name, scalar_type, values, **options):
    return {"domain": "vertex", "name": name, "semantic": "custom", "set_index": None,
            "components": 1, "scalar_type": scalar_type, "values": values, "offsets": None,
            "present": None, "metadata": {}, **options}


def payload(record):
    return {**record, "positions": list(record["positions"]), "face_offsets": list(record["face_offsets"]),
            "corner_vertices": list(record["corner_vertices"]), "attributes": [
                {**row, "values": list(row["values"]), "metadata": dict(row["metadata"]),
                 "offsets": None if row["offsets"] is None else list(row["offsets"]),
                 "present": None if row["present"] is None else list(row["present"])} for row in record["attributes"]]}


class MeshTests(unittest.TestCase):
    def test_installed_version_and_empty_mesh(self):
        self.assertEqual(__version__, importlib.metadata.version("meshvale-geometry"))
        mesh = Mesh()
        self.assertEqual((mesh.vertex_count, mesh.face_count, mesh.corner_count), (0,0,0))
        self.assertEqual(mesh.inspect_storage(), [])
        record = mesh.to_record()
        self.assertEqual(list(record["face_offsets"]), [0])
        self.assertEqual(Mesh.from_record(record).inspect_storage(), [])
        self.assertEqual(mesh.inspect_topology()["topology"]["boundaries"], [])

    def test_all_scalar_kinds_without_coercion(self):
        record = triangle()
        kinds = [("float32","f",[0.5,-1.25,2]), ("float64","d",[0.1,1e-200,-1e100]),
                 ("int32","i",[-2**31,0,2**31-1]), ("uint8","B",[0,1,255]),
                 ("uint16","H",[0,1,65535]), ("uint32","I",[0,1,2**32-1]),
                 ("uint64","Q",[0,2**53+1,2**64-1])]
        record["attributes"] = [attribute(kind,kind,array(code,values), metadata={"name":"绘制\0☃"})
                                for kind,code,values in kinds]
        mesh = Mesh.from_record(record)
        self.assertEqual(mesh.inspect_storage(), [])
        exported = mesh.to_record()
        self.assertEqual(payload(exported), payload(record))
        for row, (_,code,_) in zip(exported["attributes"],kinds):
            self.assertEqual(row["values"].format,code)
            self.assertTrue(row["values"].readonly)
        self.assertEqual(payload(Mesh.from_record(exported).to_record()), payload(record))

    def test_multiple_uvs_variable_influences_and_missingness(self):
        record = triangle()
        record["attributes"] = [
            attribute("uv0","float64",array("d",[0,0, 1,0, 0,1]), domain="corner", semantic="uv", set_index=0, components=2),
            attribute("uv1","float32",array("f",[.25,.5, .5,.75, 1,0]), domain="corner", semantic="uv", set_index=1, components=2, present=array("B",[1,0,1])),
            attribute("joints","uint32",array("I",[0,1,2,3,4, 9]), semantic="joint_index", offsets=array("Q",[0,5,5,6])),
            attribute("weights","float32",array("f",[.1,.2,.3,.15,.25, 1]), semantic="joint_weight", offsets=array("Q",[0,5,5,6]))]
        mesh = Mesh.from_record(record)
        self.assertEqual(mesh.inspect_storage(), [])
        self.assertEqual(payload(mesh.to_record()), payload(record))

    def test_import_export_lifetimes_and_immutability(self):
        source = triangle()
        source["attributes"] = [attribute("tag","uint8",array("B",[1,2,3]), metadata={"source":"original"})]
        expected = payload(source)
        borrowed = memoryview(source["positions"])
        source["positions"] = borrowed
        mesh = Mesh.from_record(source)
        exported = mesh.to_record()
        borrowed[0] = 99
        borrowed.release()
        source["attributes"][0]["values"][0] = 255
        source["attributes"][0]["metadata"]["source"] = "changed"
        del source
        gc.collect()
        self.assertEqual(payload(mesh.to_record()), expected)
        self.assertEqual(payload(exported), expected)
        with self.assertRaises(TypeError): exported["positions"][0] = 77
        with self.assertRaises(AttributeError): mesh.face_count = 100
        exported["attributes"][0]["metadata"]["source"] = "independent"
        self.assertEqual(payload(mesh.to_record()), expected)
        snapshot = Mesh.from_record(mesh.to_record())
        survivor = mesh.to_record()["positions"]
        del mesh
        gc.collect()
        self.assertEqual(list(survivor), expected["positions"])
        self.assertEqual(payload(snapshot.to_record()), expected)
        for _ in range(100): Mesh.from_record(snapshot.to_record()).inspect_topology()

    def test_position_bytes_preserve_nonfinite_payloads(self):
        expected = struct.pack("=6Q", 0x8000000000000000, 0x7ff8123456789abc,
                               0x7ff0123456789abc, 0x3ff0000000000000,
                               0x4000000000000000, 0x4008000000000000)
        positions = array("d")
        positions.frombytes(expected)
        source = triangle()
        source["positions"] = positions
        source["face_offsets"] = array("Q", [7, 2])
        source["corner_vertices"] = array("Q", [99])
        snapshot = Mesh.from_record(source)
        exported = snapshot.to_record()
        copied = Mesh.from_record(exported)
        positions[0] = 9.0
        del source, snapshot, exported
        gc.collect()
        self.assertTrue(copied.inspect_storage())
        survived = copied.to_record()
        self.assertEqual(survived["positions"].tobytes(), expected)
        self.assertEqual(list(survived["face_offsets"]), [7, 2])
        self.assertEqual(list(survived["corner_vertices"]), [99])

    def test_polygon_nonmanifold_and_detached_inspection(self):
        source = triangle()
        source["positions"].extend([0,-1,0, 0,0,1])
        source["face_offsets"] = array("Q",[0,4,7,10])
        source["corner_vertices"] = array("Q",[0,1,2,3, 1,0,3, 0,1,4])
        mesh = Mesh.from_record(source)
        result = mesh.inspect_topology()
        codes = {issue["code"] for issue in result["diagnostics"]}
        self.assertIn("topology.edge_nonmanifold",codes)
        edge = next(edge for edge in result["topology"]["edges"] if edge["vertices"] == [0,1])
        self.assertEqual(edge["corners"],[0,4,7])
        self.assertEqual(result["topology"]["face_components"],[[0,1,2]])
        self.assertEqual(result["topology"]["vertices"][0]["kind"],"nonmanifold")
        self.assertEqual(len(result["topology"]["corners"]),10)
        result["topology"]["corners"].clear()
        del mesh
        gc.collect()
        self.assertEqual(edge["corners"],[0,4,7])

    def test_mesh_defects_remain_inspectable(self):
        source = triangle()
        source["corner_vertices"][0] = 2**64-1
        source["attributes"] = [attribute("bad","float32",array("f"),components=0)]
        mesh = Mesh.from_record(source)
        self.assertIn("mesh.vertex_range", {item["code"] for item in mesh.inspect_storage()})
        self.assertIn("attribute.zero_components", {item["code"] for item in mesh.inspect_storage()})
        result = mesh.inspect_topology()
        self.assertIsNone(result["topology"])
        self.assertEqual(next(check for check in result["checks"] if check["name"] == "edge_incidence")["status"],"blocked")
        source = triangle(); source["positions"][0] = float("nan")
        result = Mesh.from_record(source).inspect_topology()
        self.assertIsNotNone(result["topology"])
        self.assertEqual(next(check for check in result["checks"] if check["name"] == "self_intersection")["status"],"unsupported")
        for offsets in [[],[1,3],[0,2],[0,3,2]]:
            source = triangle(); source["face_offsets"] = array("Q",offsets)
            self.assertTrue(Mesh.from_record(source).inspect_storage())

    def test_reject_incompatible_records(self):
        variants = [{**triangle(),"schema":"meshvale.mesh/2"}, {**triangle(),"extra":1},
                    {**triangle(),"positions":array("f",[0]*9)},
                    {**triangle(),"positions":memoryview(array("d",[0]*18))[::2]},
                    {**triangle(),"positions":memoryview(array("d",[0]*9)).cast("B").cast("d",shape=[3,3])},
                    {**triangle(),"positions":array("d",[0]*8)},
                    {**triangle(),"corner_vertices":array("q",[0,1,2])},
                    {**triangle(),"attributes":()}]
        for source in variants:
            with self.subTest(keys=source.keys()), self.assertRaises((TypeError,ValueError)):
                Mesh.from_record(source)
        for field, value in [("components",True),("components",2**32),("components",-1),
                             ("set_index",True),("set_index",2**32),("domain","edge"),
                             ("name",123),("metadata",{"tag":7}),("scalar_type","uint128"),
                             ("values",array("I",[0,0,0])),("extra",0)]:
            source = triangle(); row = attribute("a","float32",array("f",[0,0,0])); row[field] = value
            source["attributes"] = [row]
            with self.subTest(field=field), self.assertRaises((TypeError,ValueError,OverflowError)):
                Mesh.from_record(source)
        source = triangle(); del source["schema"]
        with self.assertRaises(ValueError): Mesh.from_record(source)
        with self.assertRaises(TypeError): Mesh.from_record(None)


if __name__ == "__main__":
    unittest.main()
