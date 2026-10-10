# SPDX-License-Identifier: Apache-2.0
"""Check this independent record wheel's exact payload and copied notices."""
import argparse
from pathlib import Path
import zipfile


def inspect(path):
    root = Path(__file__).resolve().parents[2]
    with zipfile.ZipFile(path) as archive:
        names = [member.filename for member in archive.infolist() if not member.is_dir()]
        metadata = [name for name in names if name.endswith(".dist-info/METADATA")]
        assert len(metadata) == 1, metadata
        prefix = metadata[0].removesuffix("METADATA")
        assert prefix.startswith("meshvale_record_consumer-") and "/" not in prefix[:-1], prefix
        modules = [name for name in names if name.startswith("meshvale_record_consumer.")
                   and name.endswith((".pyd", ".so")) and "/" not in name]
        assert len(modules) == 1, modules
        notices = {"LICENSE": root/"LICENSE", "NOTICE": root/"NOTICE"}
        notices.update({name: root/"licenses"/name for name in [
            "nanobind.txt", "robin-map.txt", "eigen-mpl2.txt",
            "eigen-apache.txt", "eigen-notices.txt"]})
        expected = {modules[0], *(prefix+name for name in ["METADATA", "WHEEL", "RECORD"]),
                    *(prefix+"licenses/"+name for name in notices)}
        assert len(names) == 11 and set(names) == expected, names
        for name, source in notices.items():
            assert archive.read(prefix+"licenses/"+name) == source.read_bytes(), name
    print(f"Record consumer package check passed: {path.name}; 11 files; exact copied notices")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wheel", type=Path)
    inspect(parser.parse_args().wheel)
