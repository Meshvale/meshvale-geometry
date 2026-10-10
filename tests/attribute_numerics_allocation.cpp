// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/attribute_numerics.h>
#include <meshvale/geometry/execution.h>

#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <vector>

#include "allocation_failure.h"

using namespace meshvale::geometry;
namespace {
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void FailEveryAllocation(bool parallel) {
  Attribute source;
  source.name = "weights";
  source.components = 2;
  source.values = std::vector<double>(4096, 0.25);
  source.offsets = std::vector<index_t>(2049);
  for (std::size_t i = 0; i < source.offsets->size(); ++i)
    (*source.offsets)[i] = i * 2;
  source.present = std::vector<std::uint8_t>(2048, 1);
  const auto original = std::get<std::vector<double>>(source.values);
  ExecutionContext e({.worker_budget = parallel ? 2U : 1U});
  std::size_t refused = 0;
  bool completed = false;
  for (std::size_t i = 0; i < 32; ++i) {
    allocation_test::allocations_before_failure = i;
    try {
      auto result = ComputeAttributeRowSums(source, 2048, {}, e);
      allocation_test::allocations_before_failure.reset();
      Require(result.Status() == AttributeReductionStatus::kAccepted,
              "allocation probe returned expected failure");
      Require(std::get<double>(result.Get(0)) == 0.5,
              "accepted result damaged");
      result = {};
      completed = true;
    } catch (const std::bad_alloc&) {
      allocation_test::allocations_before_failure.reset();
      ++refused;
    } catch (...) {
      allocation_test::allocations_before_failure.reset();
      throw;
    }
    Require(e.ActiveWorkers() == 0 && e.ActiveTrackedPayload() == 0,
            "allocation failure leaked worker or payload");
    Require(std::memcmp(original.data(),
                        std::get<std::vector<double>>(source.values).data(),
                        original.size() * sizeof(double)) == 0,
            "allocation failure changed source");
    if (completed) break;
  }
  Require(completed && refused >= 7,
          "did not reach capture/output/worker allocation boundaries");
  auto retained = ComputeAttributeRowSums(source, 2048, {}, e);
  const auto active = e.ActiveTrackedPayload();
  allocation_test::allocations_before_failure = 0;
  bool export_failed = false;
  try {
    (void)retained.CopyValues();
  } catch (const std::bad_alloc&) {
    export_failed = true;
  }
  allocation_test::allocations_before_failure.reset();
  Require(export_failed && e.ActiveTrackedPayload() == active &&
              std::get<double>(retained.Get(0)) == 0.5,
          "failed caller export changed owned result");
  std::cout << (parallel ? "parallel" : "serial")
            << " ordinary allocation boundaries refused: " << refused
            << "; capture/output/header rollback and retained export passed\n";
}
}  // namespace
int main() {
  try {
    FailEveryAllocation(false);
    FailEveryAllocation(true);
    return 0;
  } catch (const std::exception& e) {
    allocation_test::allocations_before_failure.reset();
    std::cerr << e.what() << '\n';
    return 1;
  }
}
