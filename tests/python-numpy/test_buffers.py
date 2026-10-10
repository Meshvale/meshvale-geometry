# SPDX-License-Identifier: Apache-2.0
"""Optional buffer producer tests; NumPy is not a package runtime dependency."""
import gc
import unittest
import numpy as np
from meshvale_geometry import Mesh


def source():
    return {"schema":"meshvale.mesh/1", "positions":np.array([0,0,0, 1,0,0, 0,1,0],dtype=np.float64),
            "face_offsets":np.array([0,3],dtype=np.uint64), "corner_vertices":np.array([0,1,2],dtype=np.uint64),
            "attributes":[]}


class NumpyBuffers(unittest.TestCase):
    def test_owned_numeric_input_and_readonly_array_output(self):
        record = source()
        mesh = Mesh.from_record(record)
        record["positions"][0] = 77
        exported = mesh.to_record()
        values = np.frombuffer(exported["positions"],dtype=np.float64)
        self.assertFalse(values.flags.writeable)
        self.assertEqual(values[0],0)
        del mesh,record,exported
        gc.collect()
        self.assertEqual(values.tolist(),[0,0,0, 1,0,0, 0,1,0])

    def test_unaligned_buffer_is_copied_without_pointer_casting(self):
        record = source()
        positions = np.ndarray((9,),dtype=np.float64,buffer=bytearray(9*8+1),offset=1)
        positions[:] = record["positions"]
        self.assertFalse(positions.flags.aligned)
        record["positions"] = positions
        self.assertEqual(Mesh.from_record(record).inspect_storage(),[])

    def test_incompatible_endian_stride_shape_and_signedness(self):
        native = source()["positions"]
        foreign = ">f8" if np.little_endian else "<f8"
        for buffer in [native.astype(foreign), native.reshape(3,3), np.zeros(18,dtype=np.float64)[::2],
                       native[::-1], native.astype(np.float32)]:
            record = source(); record["positions"] = buffer
            with self.subTest(format=memoryview(buffer).format),self.assertRaises(ValueError):
                Mesh.from_record(record)
        for dtype in [np.int64,np.uint32]:
            record = source(); record["corner_vertices"] = record["corner_vertices"].astype(dtype)
            with self.assertRaises(ValueError): Mesh.from_record(record)

    def test_all_supported_attribute_dtypes_round_trip(self):
        record = source()
        for dtype in [np.float32,np.float64,np.int32,np.uint8,np.uint16,np.uint32,np.uint64]:
            values = np.array([0,1,2],dtype=dtype)
            record["attributes"].append({"domain":"vertex","name":values.dtype.name,"semantic":"custom",
                "set_index":None,"components":1,"scalar_type":values.dtype.name,"values":values,
                "offsets":None,"present":np.array([1,0,1],dtype=np.uint8),"metadata":{}})
        exported = Mesh.from_record(record).to_record()
        for old,new in zip(record["attributes"],exported["attributes"]):
            restored = np.frombuffer(new["values"],dtype=old["values"].dtype)
            np.testing.assert_array_equal(restored,old["values"])
            self.assertFalse(restored.flags.writeable)


if __name__ == "__main__":
    unittest.main()
