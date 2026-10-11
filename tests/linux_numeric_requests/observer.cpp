// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/scalar_buffer.h>
#include <meshvale/geometry/vector_buffer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <utility>

#include "materialization_allocation.h"
#include "numeric_buffer_internal.h"
#include "trace.h"
#if !defined(__clang__) || !defined(__linux__)
#error This original-route observer requires Linux Clang.
#endif
namespace {
namespace geo = meshvale::geometry;
namespace detail = geo::materialization_detail;
using observation::Mode;
using observation::Phase;
constexpr std::size_t kRequestBudget = 1024 * 1024;
constexpr std::size_t kCalibrationMallocBytes = 37;
constexpr std::size_t kCalibrationNewBytes = 19;
constexpr std::size_t kCalibrationCallocCount = 3;
constexpr std::size_t kCalibrationReallocatedBytes = 53;
constexpr std::size_t kCalibrationAlignment = 64;
constexpr int kScalarWidth = 1;
constexpr std::array<std::size_t, 4> kCounts{0, 1, 7, 17};
constexpr const char* kControlShape = "control";
constexpr const char* kSeedShape = "operation-seed";
constexpr const char* kPositionShape = "vector-f64x3";
constexpr const char* kFloatShape = "scalar-f32";
constexpr const char* kDoubleShape = "scalar-f64";
constexpr const char* kSigned32Shape = "scalar-i32";
constexpr const char* kUnsigned8Shape = "scalar-u8";
constexpr const char* kUnsigned16Shape = "scalar-u16";
constexpr const char* kUnsigned32Shape = "scalar-u32";
constexpr const char* kUnsigned64Shape = "scalar-u64";
constexpr int kSuccess = 0;
constexpr int kFailure = 1;
bool Calibration() {
  observation::Begin();
  observation::SetPhase(Phase::kCalibration);
  void* (*volatile malloc_call)(std::size_t) = &std::malloc;
  void* (*volatile calloc_call)(std::size_t, std::size_t) = &std::calloc;
  void* (*volatile realloc_call)(void*, std::size_t) = &std::realloc;
  void (*volatile free_call)(void*) = &std::free;
  void* first = malloc_call(kCalibrationMallocBytes);
  void* grown = realloc_call(first, kCalibrationReallocatedBytes);
  free_call(grown ? grown : first);
  void* zeroed = calloc_call(kCalibrationCallocCount, kCalibrationMallocBytes);
  free_call(zeroed);
  void* ordinary = ::operator new(kCalibrationNewBytes);
  ::operator delete(ordinary);
  void* aligned = ::operator new(kCalibrationNewBytes,
                                 std::align_val_t{kCalibrationAlignment});
  ::operator delete(aligned, std::align_val_t{kCalibrationAlignment});
  return observation::End(
      {kControlShape, 1, kScalarWidth, 0, 0, 0, false, Mode::kControl});
}
std::size_t ObserveSeed(const geo::ExecutionContext& context, bool& good) {
  observation::Begin(&context);
  observation::SetPhase(Phase::kSeed);
  std::size_t seed;
  {
    auto operation =
        detail::AllocationContext(context, {}).WithRequestLimit(kRequestBudget);
    seed = operation.OperationActiveBytes();
    observation::SetPhase(Phase::kSeedDestroy);
  }
  good = observation::End(
             {kSeedShape, 1, kScalarWidth, 0, seed, 0, false, Mode::kSeed}) &&
         good;
  observation::Begin(&context);
  observation::SetPhase(Phase::kDenied);
  bool denied = false;
  try {
    (void)detail::AllocationContext(context, {}).WithRequestLimit(seed - 1);
  } catch (const detail::AdmissionFailure& failure) {
    denied = failure.Code() == detail::AdmissionCode::kBudgetExceeded;
  }
  observation::Sample();
  good = observation::End({kSeedShape, 1, kScalarWidth, 0, seed, 0, false,
                           Mode::kSeedDenied}) &&
         denied && good;
  return seed;
}
template <class Construct, class GetScalars>
bool Numeric(const char* shape, std::size_t scalar_bytes, int width,
             std::size_t count, const geo::numeric_detail::RequestPlan& plan,
             const geo::ExecutionContext& context, std::size_t seed,
             Construct construct, GetScalars get_scalars, bool deny) {
  const auto requested = plan.implementation_bytes + plan.numerical_bytes;
  auto operation = detail::AllocationContext(context, {})
                       .WithRequestLimit(seed + requested - (deny ? 1 : 0));
  observation::Case observed{shape,
                             scalar_bytes,
                             width,
                             count,
                             plan.implementation_bytes,
                             plan.numerical_bytes,
                             plan.qualified,
                             deny ? Mode::kDenied : Mode::kAdmitted};
  observation::Begin(&context, &operation);
  bool correct = true;
  if (deny) {
    observation::SetPhase(Phase::kDenied);
    correct = false;
    try {
      (void)operation.ReserveOriginalRequest(requested);
    } catch (const detail::AdmissionFailure& failure) {
      correct = failure.Code() == detail::AdmissionCode::kBudgetExceeded;
    }
    observation::Sample();
  } else {
    {
      auto lease = operation.ReserveOriginalRequest(requested);
      observation::SetPhase(Phase::kAdmitted);
      observation::Sample();
      observation::SetPhase(Phase::kConstruct);
      {
        auto owner = construct(count);
        auto scalars = get_scalars(owner);
        observed.numeric_data =
            reinterpret_cast<std::uintptr_t>(scalars.data());
        correct = scalars.size() == count * static_cast<std::size_t>(width);
        // Actual unpublished owners are fully initialized without Eigen
        // expressions; the test records only original allocation/free routes.
        for (auto& value : scalars) value = 0;
        observation::SetPhase(Phase::kDestroy);
      }
      observation::SetPhase(Phase::kOwnerFreed);
      observation::Sample();
    }
    observation::SetPhase(Phase::kLeaseFreed);
    observation::Sample();
  }
  return observation::End(observed) && correct;
}
template <geo::SupportedAttributeScalar T>
bool Scalar(const char* shape, std::size_t count,
            const geo::ExecutionContext& context, std::size_t seed) {
  const auto plan = geo::numeric_detail::Access::ScalarPlan<T>(count);
  auto construct = [](std::size_t count) {
    return geo::numeric_detail::Access::AllocateScalars<T>(count);
  };
  auto scalars = [](geo::ScalarBuffer<T>& owner) { return owner.Values(); };
  bool good = Numeric(shape, sizeof(T), kScalarWidth, count, plan, context,
                      seed, construct, scalars, false);
  if (count != 0)
    good = Numeric(shape, sizeof(T), kScalarWidth, count, plan, context, seed,
                   construct, scalars, true) &&
           good;
  return good;
}
bool Position(std::size_t count, const geo::ExecutionContext& context,
              std::size_t seed) {
  const auto plan = geo::numeric_detail::Access::PositionPlan(count);
  auto construct = [](std::size_t count) {
    return geo::numeric_detail::Access::AllocatePositions(count);
  };
  auto scalars = [](auto& owner) {
    return geo::numeric_detail::Access::PositionScalars(owner);
  };
  bool good = Numeric(kPositionShape, sizeof(double), geo::kVector3Components,
                      count, plan, context, seed, construct, scalars, false);
  if (count != 0)
    good = Numeric(kPositionShape, sizeof(double), geo::kVector3Components,
                   count, plan, context, seed, construct, scalars, true) &&
           good;
  return good;
}
}  // namespace
int main() {
  std::printf("PROFILE\t1\t%s\t%s\tCLANG=%s\tASAN=%d\tTSAN=%d\n",
              OBSERVER_PROFILE, OBSERVER_GEOMETRY_REVISION, __clang_version__,
              __has_feature(address_sanitizer),
              __has_feature(thread_sanitizer));
  std::printf("SOURCES\t%s\t%s\t%s\n", OBSERVER_SCALAR_SOURCE_HASH,
              OBSERVER_VECTOR_SOURCE_HASH, OBSERVER_ALLOCATION_SOURCE_HASH);
  bool good = Calibration();
  geo::ExecutionOptions options;
  options.tracked_payload_budget_bytes = kRequestBudget;
  geo::ExecutionContext context(options);
  const auto seed = ObserveSeed(context, good);
  for (const auto count : kCounts) {
    good = Scalar<float>(kFloatShape, count, context, seed) && good;
    good = Scalar<double>(kDoubleShape, count, context, seed) && good;
    good = Scalar<std::int32_t>(kSigned32Shape, count, context, seed) && good;
    good = Scalar<std::uint8_t>(kUnsigned8Shape, count, context, seed) && good;
    good =
        Scalar<std::uint16_t>(kUnsigned16Shape, count, context, seed) && good;
    good =
        Scalar<std::uint32_t>(kUnsigned32Shape, count, context, seed) && good;
    good =
        Scalar<std::uint64_t>(kUnsigned64Shape, count, context, seed) && good;
    good = Position(count, context, seed) && good;
  }
  return good && context.ActiveTrackedPayload() == 0 ? kSuccess : kFailure;
}
