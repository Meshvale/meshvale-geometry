// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_TESTS_ALLOCATION_FAILURE_H_
#define MESHVALE_GEOMETRY_TESTS_ALLOCATION_FAILURE_H_

#include <cstddef>

void operator delete(void* pointer, std::size_t size) noexcept;
void operator delete[](void* pointer, std::size_t size) noexcept;

namespace meshvale::geometry::allocation_test {
// Only ordinary new/new[] are intercepted. Thread/exception runtime and aligned
// allocation paths are outside this focused probe.
extern thread_local bool reject_allocations;
}  // namespace meshvale::geometry::allocation_test
#endif  // MESHVALE_GEOMETRY_TESTS_ALLOCATION_FAILURE_H_
