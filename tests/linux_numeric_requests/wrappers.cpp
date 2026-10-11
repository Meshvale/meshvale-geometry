// SPDX-License-Identifier: Apache-2.0
// Test-only linker delegation; originals remain undefined for --wrap
// resolution.
#include <cstddef>
#include <cstdint>
#include <new>

#include "trace.h"

extern "C" void* __real_malloc(std::size_t);
extern "C" void __real_free(void*);
extern "C" void* __real_calloc(std::size_t, std::size_t);
extern "C" void* __real_realloc(void*, std::size_t);
extern "C" void* __wrap_malloc(std::size_t bytes) {
  observation::Record(observation::Route::kMalloc, observation::Action::kBegin,
                      bytes, 0, 0);
  void* result = __real_malloc(bytes);
  observation::Record(observation::Route::kMalloc, observation::Action::kEnd,
                      bytes, 0, reinterpret_cast<std::uintptr_t>(result));
  return result;
}
extern "C" void __wrap_free(void* pointer) {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  observation::Record(observation::Route::kFree, observation::Action::kBegin, 0,
                      0, address);
  __real_free(pointer);
  observation::Record(observation::Route::kFree, observation::Action::kEnd, 0,
                      0, address);
}
extern "C" void* __wrap_calloc(std::size_t count, std::size_t size) {
  observation::Record(observation::Route::kCalloc, observation::Action::kBegin,
                      count, size, 0);
  void* result = __real_calloc(count, size);
  observation::Record(observation::Route::kCalloc, observation::Action::kEnd,
                      count, size, reinterpret_cast<std::uintptr_t>(result));
  return result;
}
extern "C" void* __wrap_realloc(void* pointer, std::size_t bytes) {
  const auto previous = reinterpret_cast<std::uintptr_t>(pointer);
  observation::Record(observation::Route::kRealloc, observation::Action::kBegin,
                      bytes, 0, 0, previous);
  void* result = __real_realloc(pointer, bytes);
  observation::Record(observation::Route::kRealloc, observation::Action::kEnd,
                      bytes, 0, reinterpret_cast<std::uintptr_t>(result),
                      previous);
  return result;
}

extern "C" void* __real__Znwm(std::size_t bytes);
extern "C" void* __wrap__Znwm(std::size_t bytes) {
  observation::Record(observation::Route::kNew, observation::Action::kBegin,
                      bytes, 0, 0);
  void* result = __real__Znwm(bytes);
  observation::Record(observation::Route::kNew, observation::Action::kEnd,
                      bytes, 0, reinterpret_cast<std::uintptr_t>(result));
  return result;
}
extern "C" void* __real__Znam(std::size_t bytes);
extern "C" void* __wrap__Znam(std::size_t bytes) {
  observation::Record(observation::Route::kNewArray,
                      observation::Action::kBegin, bytes, 0, 0);
  void* result = __real__Znam(bytes);
  observation::Record(observation::Route::kNewArray, observation::Action::kEnd,
                      bytes, 0, reinterpret_cast<std::uintptr_t>(result));
  return result;
}
extern "C" void* __real__ZnwmSt11align_val_t(std::size_t bytes,
                                             std::align_val_t alignment);
extern "C" void* __wrap__ZnwmSt11align_val_t(std::size_t bytes,
                                             std::align_val_t alignment) {
  observation::Record(observation::Route::kNewAligned,
                      observation::Action::kBegin, bytes,
                      static_cast<std::size_t>(alignment), 0);
  void* result = __real__ZnwmSt11align_val_t(bytes, alignment);
  observation::Record(observation::Route::kNewAligned,
                      observation::Action::kEnd, bytes,
                      static_cast<std::size_t>(alignment),
                      reinterpret_cast<std::uintptr_t>(result));
  return result;
}
extern "C" void* __real__ZnamSt11align_val_t(std::size_t bytes,
                                             std::align_val_t alignment);
extern "C" void* __wrap__ZnamSt11align_val_t(std::size_t bytes,
                                             std::align_val_t alignment) {
  observation::Record(observation::Route::kNewArrayAligned,
                      observation::Action::kBegin, bytes,
                      static_cast<std::size_t>(alignment), 0);
  void* result = __real__ZnamSt11align_val_t(bytes, alignment);
  observation::Record(observation::Route::kNewArrayAligned,
                      observation::Action::kEnd, bytes,
                      static_cast<std::size_t>(alignment),
                      reinterpret_cast<std::uintptr_t>(result));
  return result;
}
extern "C" void __real__ZdlPv(void* pointer) noexcept;
extern "C" void __wrap__ZdlPv(void* pointer) noexcept {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  observation::Record(observation::Route::kDelete, observation::Action::kBegin,
                      0, 0, address);
  __real__ZdlPv(pointer);
  observation::Record(observation::Route::kDelete, observation::Action::kEnd, 0,
                      0, address);
}
extern "C" void __real__ZdaPv(void* pointer) noexcept;
extern "C" void __wrap__ZdaPv(void* pointer) noexcept {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  observation::Record(observation::Route::kDeleteArray,
                      observation::Action::kBegin, 0, 0, address);
  __real__ZdaPv(pointer);
  observation::Record(observation::Route::kDeleteArray,
                      observation::Action::kEnd, 0, 0, address);
}
extern "C" void __real__ZdlPvm(void* pointer, std::size_t bytes) noexcept;
extern "C" void __wrap__ZdlPvm(void* pointer, std::size_t bytes) noexcept {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  observation::Record(observation::Route::kDeleteSized,
                      observation::Action::kBegin, bytes, 0, address);
  __real__ZdlPvm(pointer, bytes);
  observation::Record(observation::Route::kDeleteSized,
                      observation::Action::kEnd, bytes, 0, address);
}
extern "C" void __real__ZdaPvm(void* pointer, std::size_t bytes) noexcept;
extern "C" void __wrap__ZdaPvm(void* pointer, std::size_t bytes) noexcept {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  observation::Record(observation::Route::kDeleteArraySized,
                      observation::Action::kBegin, bytes, 0, address);
  __real__ZdaPvm(pointer, bytes);
  observation::Record(observation::Route::kDeleteArraySized,
                      observation::Action::kEnd, bytes, 0, address);
}
extern "C" void __real__ZdlPvSt11align_val_t(
    void* pointer, std::align_val_t alignment) noexcept;
extern "C" void __wrap__ZdlPvSt11align_val_t(
    void* pointer, std::align_val_t alignment) noexcept {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  observation::Record(observation::Route::kDeleteAligned,
                      observation::Action::kBegin, 0,
                      static_cast<std::size_t>(alignment), address);
  __real__ZdlPvSt11align_val_t(pointer, alignment);
  observation::Record(observation::Route::kDeleteAligned,
                      observation::Action::kEnd, 0,
                      static_cast<std::size_t>(alignment), address);
}
extern "C" void __real__ZdaPvSt11align_val_t(
    void* pointer, std::align_val_t alignment) noexcept;
extern "C" void __wrap__ZdaPvSt11align_val_t(
    void* pointer, std::align_val_t alignment) noexcept {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  observation::Record(observation::Route::kDeleteArrayAligned,
                      observation::Action::kBegin, 0,
                      static_cast<std::size_t>(alignment), address);
  __real__ZdaPvSt11align_val_t(pointer, alignment);
  observation::Record(observation::Route::kDeleteArrayAligned,
                      observation::Action::kEnd, 0,
                      static_cast<std::size_t>(alignment), address);
}
extern "C" void __real__ZdlPvmSt11align_val_t(
    void* pointer, std::size_t bytes, std::align_val_t alignment) noexcept;
extern "C" void __wrap__ZdlPvmSt11align_val_t(
    void* pointer, std::size_t bytes, std::align_val_t alignment) noexcept {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  observation::Record(observation::Route::kDeleteSizedAligned,
                      observation::Action::kBegin, bytes,
                      static_cast<std::size_t>(alignment), address);
  __real__ZdlPvmSt11align_val_t(pointer, bytes, alignment);
  observation::Record(observation::Route::kDeleteSizedAligned,
                      observation::Action::kEnd, bytes,
                      static_cast<std::size_t>(alignment), address);
}
extern "C" void __real__ZdaPvmSt11align_val_t(
    void* pointer, std::size_t bytes, std::align_val_t alignment) noexcept;
extern "C" void __wrap__ZdaPvmSt11align_val_t(
    void* pointer, std::size_t bytes, std::align_val_t alignment) noexcept {
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  observation::Record(observation::Route::kDeleteArraySizedAligned,
                      observation::Action::kBegin, bytes,
                      static_cast<std::size_t>(alignment), address);
  __real__ZdaPvmSt11align_val_t(pointer, bytes, alignment);
  observation::Record(observation::Route::kDeleteArraySizedAligned,
                      observation::Action::kEnd, bytes,
                      static_cast<std::size_t>(alignment), address);
}
