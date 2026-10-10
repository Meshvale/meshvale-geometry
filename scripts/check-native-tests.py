# SPDX-License-Identifier: Apache-2.0
"""Verify the exact native suite selected for an allocation-runtime profile."""
import argparse
import json
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--configuration', default='Release')
    parser.add_argument('--allocation-probe', choices=['enabled', 'disabled'],
                        default='enabled')
    args = parser.parse_args()
    tests = json.loads(subprocess.check_output([
        'ctest', '--test-dir', args.build_dir, '-C', args.configuration,
        '--show-only=json-v1'], text=True))['tests']
    names = [test['name'] for test in tests]
    expected = {'attribute_storage', 'polygon_topology', 'pooled_editing',
                'shared_execution_leases', 'exact_polygon_triangulation',
                'triangulation_fraction_oracle', 'checked_attribute_numerics',
                'eigen_scalar_storage', 'editor_traversal_batch'}
    if args.allocation_probe == 'enabled':
        expected.update({'execution_allocation_failure',
                         'attribute_numerics_allocation_failure',
                         'editor_batch_allocation_failure'})
    if len(names) != len(expected) or set(names) != expected:
        parser.error(f'expected {sorted(expected)}, found {names}')
    print(f'Native test inventory passed: {len(names)} suites; '
          f'allocation probe {args.allocation_probe}; {names}')


if __name__ == '__main__':
    main()
