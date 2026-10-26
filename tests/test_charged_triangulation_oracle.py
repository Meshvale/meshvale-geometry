# SPDX-License-Identifier: Apache-2.0
"""Public-output rational proof and complete typed backing, for both real routes."""
import copy
import json
from pathlib import Path
import subprocess
import sys

from triangulation_fraction_oracle import verify


def main():
    driver = Path(sys.argv[1]).resolve(strict=True)
    results = {}
    for route in ("legacy", "retained"):
        for workers in ("1", "4"):
            run = subprocess.run([str(driver), route, workers], check=True,
                                 capture_output=True, text=True)
            result = json.loads(run.stdout)
            source = result["source"]
            assert len(source["positions"]) == 12
            assert source["face_offsets"] == [0, 3, 7, 12]
            assert source["corner_vertices"] == [0, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10]
            assert len(source["attributes"]) == 19
            proof = verify(source, result)
            assert proof["triangles"] == 6 and proof["channels"] == 19
            if (route, "1") in results:
                assert result == results[route, "1"], "serial/parallel bytes/maps differ"
            results[route, workers] = result
            # This independently mathematical predicate must reject actual
            # output corruption. Never weaken native expectations to make it pass.
            bad = copy.deepcopy(result)
            bad["corner_sources"][0] = 11  # outside first authored face
            try:
                verify(source, bad)
            except AssertionError:
                pass
            else:
                raise AssertionError("corrupted authored correspondence accepted")
    print("Four actual charged routes: exact Fraction geometry and all nineteen channel backings passed")


if __name__ == "__main__":
    main()
