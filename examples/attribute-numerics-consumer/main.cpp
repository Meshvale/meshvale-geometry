// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/attribute_numerics.h>
#include <meshvale/geometry/execution.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace meshvale::geometry;
int main() {
  ExecutionContext execution({.worker_budget = 2});
  Attribute weights;
  weights.name = "skin_weights";
  weights.semantic = "joint_weights";
  weights.offsets = std::vector<index_t>{0, 5, 5, 7};
  weights.present = std::vector<std::uint8_t>{1, 1, 0};
  weights.values = std::vector<double>{0.25, 0.5, 0.25, 0.25, 0.25, 3, 4};
  auto sums = ComputeAttributeRowSums(weights, 3, {}, execution);
  if (sums.Status() != AttributeReductionStatus::kAccepted ||
      std::get<double>(sums.Get(0)) != 1.5 || sums.Presence()[1] != 1 ||
      sums.Presence()[2] != 0)
    throw std::runtime_error("weight reduction");
  Attribute normals;
  normals.domain = AttributeDomain::corner;
  normals.name = "normals";
  normals.semantic = "normal";
  normals.components = 3;
  normals.values = std::vector<float>{3, 4, 0, 0, 0, 2};
  auto norms = ComputeAttributeRowSquaredNorms(normals, 2, {}, execution);
  if (std::get<float>(norms.Get(0)) != 25 || std::get<float>(norms.Get(1)) != 4)
    throw std::runtime_error("independent corner normals");
  Attribute joints;
  joints.name = "joint_indices";
  joints.values = std::vector<std::uint64_t>{9007199254740993ULL};
  auto ids = ComputeAttributeRowSums(joints, 1, {}, execution);
  if (std::get<std::uint64_t>(ids.Get(0)) != 9007199254740993ULL)
    throw std::runtime_error("integer joint encoding");
  auto owned = sums.CopyValues();
  sums = {};
  if (std::get<std::vector<double>>(owned)[0] != 1.5)
    throw std::runtime_error("owned numerical export");
  std::cout << "Installed typed row reductions, five influences, missing/empty "
               "rows, corner normals and uint64 IDs passed\n";
}
