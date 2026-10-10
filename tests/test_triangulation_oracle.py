# SPDX-License-Identifier: Apache-2.0
"""Verify native triangle coverage and authored bit correspondence independently."""
import json
from pathlib import Path
import subprocess
import sys

from triangulation_fixtures import cases, mesh, serialize
from triangulation_fraction_oracle import verify


def run(driver, fixture, workers, expected=None):
    completed = subprocess.run([str(driver), str(workers)],
                               input=serialize(fixture).encode(), capture_output=True,
                               timeout=30, check=False)
    if completed.stderr:
        raise AssertionError(completed.stderr.decode(errors='replace'))
    result = json.loads(completed.stdout)
    if expected is None:
        assert completed.returncode == 0 and result['accepted'], result
        verify(fixture, result)
    else:
        assert completed.returncode == 2 and not result['accepted'] and result['code'] == expected, result
        assert result['mesh'] is None and not result['face_sources'] and not result['corner_sources']
        assert result['source'] == fixture and result['source_unchanged'] is True
    return result


def main():
    driver = Path(sys.argv[1]).resolve()
    count = 0
    for fixture in cases():
        serial = run(driver, fixture, 1)
        parallel = run(driver, fixture, 4)
        for key in ['mesh', 'face_sources', 'corner_sources', 'face_output_offsets', 'source']:
            assert serial[key] == parallel[key], 'worker scheduling changed geometry or correspondence'
        if len(fixture['face_offsets']) > 2:
            assert parallel['workers_used'] == min(4, len(fixture['face_offsets']) - 1)
        count += 2
    for points, expected in [
        ([(0,0,0),(1,1,1),(2,2,2)], 'conversion.degenerate_face'),
        ([(0,0,0),(2,2,0),(0,2,0),(2,0,0)], 'conversion.self_intersection'),
        ([(0,0,0),(1,0,0),(1,1,5e-324),(0,1,0)], 'conversion.nonplanar_face'),
        ([(0,0,0),(2,0,0),(2,2,0),(0,0,0)], 'conversion.repeated_position'),
    ]:
        run(driver, mesh(points), 4, expected)
        count += 1
    print(f'Independent Fraction coverage/overlap/winding/maps/channel checks passed: {count} native calls')


if __name__ == '__main__':
    main()
