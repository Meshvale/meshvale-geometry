// SPDX-License-Identifier: Apache-2.0
#include "allocation_failure.h"

#include <cstdlib>
#include <new>

namespace meshvale::geometry::allocation_test {
thread_local bool reject_allocations = false;
thread_local std::optional<std::size_t> allocations_before_failure;
}  // namespace meshvale::geometry::allocation_test

// Keep the replacement bodies in a separate compilation unit: GCC13 can report
// mismatched-new-delete after inlining free into an ordinary operator-new
// caller.
void* operator new(std::size_t size) {
  if (meshvale::geometry::allocation_test::reject_allocations)
    throw std::bad_alloc();
  auto& remaining =
      meshvale::geometry::allocation_test::allocations_before_failure;
  if (remaining) {
    if (*remaining == 0) throw std::bad_alloc();
    --*remaining;
  }
  if (auto* pointer = std::malloc(size ? size : 1)) return pointer;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
