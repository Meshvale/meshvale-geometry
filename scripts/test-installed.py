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
    parser.add_argument("--reports",action="store_true",help="also test optional report validation")
    parser.add_argument("--report-dependencies",type=Path,help="offline directory of report dependency wheels")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="meshvale-installed-") as scratch:
        directory = Path(scratch)
        environment = directory/"env"
        venv.EnvBuilder(with_pip=True).create(environment)
        python = environment/("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
        run(python,"-m","pip","install","--no-index",options.geometry_wheel.resolve(),
            options.consumer_wheel.resolve(),cwd=directory)
        run(python,"-I","-c","import meshvale_reports as r,sys; assert r.schema_document()['$id']=='urn:meshvale:report-envelope:1'; "
            "assert 'meshvale_geometry' not in sys.modules and 'jsonschema' not in sys.modules; "
            "print('Declarative report consumption loads no native Geometry or validator')",cwd=directory)
        run(python,"-I","-m","unittest","discover","-s",root/"tests/python","-v",cwd=directory)
        run(python,"-I",root/"examples/python/inspect_mesh.py",cwd=directory)
        for order in ["geometry-first","consumer-first"]:
            run(python,"-I",root/"examples/record-consumer/check.py",order,cwd=directory)
        if options.numpy:
            run(python,"-m","pip","install","--only-binary=:all:","numpy>=2,<3",cwd=directory)
            run(python,"-I","-m","unittest","discover","-s",root/"tests/python-numpy","-v",cwd=directory)
        if options.reports:
            dependency_args = ["--no-index", "--find-links", options.report_dependencies.resolve()] if options.report_dependencies else []
            run(python,"-m","pip","install",*dependency_args,"jsonschema==4.26.0",cwd=directory)
            run(python,"-m","pip","check",cwd=directory)
            # Test the report-only interface with the native Geometry module actually absent.
            native = Path(subprocess.check_output([str(python),"-I","-c",
                "import meshvale_geometry as g; from pathlib import Path; print(Path(g.__file__).parent)"],cwd=directory,text=True).strip())
            native.resolve().relative_to(environment.resolve())
            for extension in [*native.glob("_geometry.*.pyd"), *native.glob("_geometry.*.so")]:
                extension.resolve().relative_to(environment.resolve())
                extension.unlink()
            run(python,"-I","-m","unittest","discover","-s",root/"tests/reports","-v",cwd=directory)
