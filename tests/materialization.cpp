// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/scalar_buffer.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <stop_token>

#include "materialization_allocation.h"
#include "numeric_buffer_internal.h"

namespace {
using namespace meshvale::geometry;
namespace allocation = materialization_detail;
constexpr std::size_t kRequestBudget = 1024 * 1024;
constexpr std::size_t kWorkerBudget = 2;
constexpr std::size_t kMinimumParallelVertices = 1;
constexpr std::size_t kScalarCount = 7;
constexpr std::size_t kPositionRows = 3;
ExecutionOptions TestExecutionOptions() {
  return {.worker_budget = kWorkerBudget,
          .minimum_parallel_vertices = kMinimumParallelVertices,
          .tracked_payload_budget_bytes = kRequestBudget};
}
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template <SupportedAttributeScalar T>
void ScalarCase() {
  ExecutionContext execution(TestExecutionOptions());
  {
    auto operation = allocation::AllocationContext(execution, {})
                         .WithRequestLimit(kRequestBudget);
    const auto seed = operation.OperationActiveBytes();
    const auto zero = numeric_detail::Access::ScalarPlan<T>(0);
    Require(zero.implementation_bytes == 0 && zero.numerical_bytes == 0,
            "zero scalar plan requested a block");
    const auto plan = numeric_detail::Access::ScalarPlan<T>(kScalarCount);
    Require(plan.qualified, "unqualified scalar test profile");
    const auto original_bytes =
        plan.implementation_bytes + plan.numerical_bytes;
    {
      auto lease = operation.ReserveOriginalRequest(original_bytes);
      auto values = numeric_detail::Access::AllocateScalars<T>(kScalarCount);
      Require(
          values.size() == kScalarCount && values.capacity() == kScalarCount,
          "uninitialized buffer extent");
      Require(execution.ActiveTrackedPayload() == seed + original_bytes,
              "original request not charged before fill");
      for (std::size_t i = 0; i < kScalarCount; ++i)
        values.data()[i] = static_cast<T>(i);
      Require(
          values.data()[kScalarCount - 1] == static_cast<T>(kScalarCount - 1),
          "private scalar destination not writable");
    }
    Require(execution.ActiveTrackedPayload() == seed,
            "original requests retained after numerical destruction");
  }
  Require(execution.ActiveTrackedPayload() == 0, "operation seed leaked");
}
void PositionCase() {
  ExecutionContext execution(TestExecutionOptions());
  {
    auto operation = allocation::AllocationContext(execution, {})
                         .WithRequestLimit(kRequestBudget);
    const auto plan = numeric_detail::Access::PositionPlan(kPositionRows);
    Require(plan.qualified, "unqualified position test profile");
    auto lease = operation.ReserveOriginalRequest(plan.implementation_bytes +
                                                  plan.numerical_bytes);
    auto positions = numeric_detail::Access::AllocatePositions(kPositionRows);
    auto scalars = numeric_detail::Access::PositionScalars(positions);
    constexpr std::uint64_t kNegativeZeroBits = UINT64_C(0x8000000000000000);
    constexpr std::uint64_t kSignalingNanBits = UINT64_C(0x7ff0000000000001);
    const auto negative_zero = std::bit_cast<double>(kNegativeZeroBits);
    const auto signaling_nan = std::bit_cast<double>(kSignalingNanBits);
    std::memcpy(&scalars[0], &negative_zero, sizeof(double));
    std::memcpy(&scalars[1], &signaling_nan, sizeof(double));
    for (std::size_t i = 2; i < scalars.size(); ++i) scalars[i] = 0;
    const auto view = numeric_detail::Access::PositionScalars(
        static_cast<const decltype(positions)&>(positions));
    Require(std::bit_cast<std::uint64_t>(view[0]) == kNegativeZeroBits &&
                std::bit_cast<std::uint64_t>(view[1]) == kSignalingNanBits,
            "raw position bits changed");
  }
  Require(execution.ActiveTrackedPayload() == 0, "position requests leaked");
}
void AdmissionCase() {
  ExecutionContext execution(TestExecutionOptions());
  const auto plan = numeric_detail::Access::ScalarPlan<double>(kScalarCount);
  const auto bytes = plan.implementation_bytes + plan.numerical_bytes;
  std::size_t seed = 0;
  {
    auto operation = allocation::AllocationContext(execution, {})
                         .WithRequestLimit(kRequestBudget);
    seed = operation.OperationActiveBytes();
  }
  {
    auto operation = allocation::AllocationContext(execution, {})
                         .WithRequestLimit(seed + bytes);
    auto lease = operation.ReserveOriginalRequest(bytes);
    Require(operation.OperationActiveBytes() == seed + bytes,
            "exact original request cap");
    bool rejected = false;
    try {
      (void)operation.ReserveOriginalRequest(1);
    } catch (const allocation::AdmissionFailure& failure) {
      rejected = failure.Code() == allocation::AdmissionCode::kBudgetExceeded;
    }
    Require(rejected && operation.OperationActiveBytes() == seed + bytes,
            "original request plus one mutated cap");
  }
  {
    std::stop_source stop;
    auto operation = allocation::AllocationContext(execution, stop.get_token())
                         .WithRequestLimit(kRequestBudget);
    const auto before = operation.OperationActiveBytes();
    stop.request_stop();
    bool canceled = false;
    try {
      (void)operation.ReserveOriginalRequest(bytes);
    } catch (const allocation::AdmissionFailure& failure) {
      canceled = failure.Code() == allocation::AdmissionCode::kCanceled;
    }
    Require(canceled && operation.OperationActiveBytes() == before,
            "canceled original request did not roll back");
  }
  Require(execution.ActiveTrackedPayload() == 0, "failed admission leaked");
}
}  // namespace
int main() {
  try {
    ScalarCase<float>();
    ScalarCase<double>();
    ScalarCase<std::int32_t>();
    ScalarCase<std::uint8_t>();
    ScalarCase<std::uint16_t>();
    ScalarCase<std::uint32_t>();
    ScalarCase<std::uint64_t>();
    PositionCase();
    AdmissionCase();
    std::cout << "Materialization original-request slice passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
