# SPDX-License-Identifier: Apache-2.0
"""Test two wheels in a fresh environment, outside the source checkout."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import venv


def run(*command, cwd):
    subprocess.run([str(part) for part in command],cwd=cwd,check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--geometry-wheel",required=True,type=Path)
    parser.add_argument("--consumer-wheel",required=True,type=Path)
    parser.add_argument("--numpy",action="store_true",help="also test optional NumPy buffer producers")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="meshvale-installed-") as scratch:
        directory = Path(scratch)
        environment = directory/"env"
        venv.EnvBuilder(with_pip=True).create(environment)
        python = environment/("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
        run(python,"-m","pip","install","--no-index",options.geometry_wheel.resolve(),
            options.consumer_wheel.resolve(),cwd=directory)
        run(python,"-I","-m","unittest","discover","-s",root/"tests/python","-v",cwd=directory)
        run(python,"-I",root/"examples/python/inspect_mesh.py",cwd=directory)
        for order in ["geometry-first","consumer-first"]:
            run(python,"-I",root/"examples/record-consumer/check.py",order,cwd=directory)
        if options.numpy:
            run(python,"-m","pip","install","--only-binary=:all:","numpy>=2,<3",cwd=directory)
            run(python,"-I","-m","unittest","discover","-s",root/"tests/python-numpy","-v",cwd=directory)
