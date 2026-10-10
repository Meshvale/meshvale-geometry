// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_TESTS_ALLOCATION_FAILURE_H_
#define MESHVALE_GEOMETRY_TESTS_ALLOCATION_FAILURE_H_

namespace meshvale::geometry::allocation_test {
// Only ordinary new/new[] are intercepted. Thread/exception runtime and aligned
// allocation paths are outside this focused probe.
extern thread_local bool reject_allocations;
}  // namespace meshvale::geometry::allocation_test
#endif  // MESHVALE_GEOMETRY_TESTS_ALLOCATION_FAILURE_H_
