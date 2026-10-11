// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_TEST_MATERIALIZATION_FIXTURE_SUPPORT_H_
#define MESHVALE_TEST_MATERIALIZATION_FIXTURE_SUPPORT_H_
#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/materialization.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
namespace materialization_test {
using namespace meshvale::geometry;
void Require(bool condition, const char* message);
MeshMaterializationLimits Limits();
MeshMaterializationWorkPolicy Work();
MeshInspectionLimits InspectionLimits(std::uint64_t issue_limit);
MaterializedMesh Capture(const MeshMaterializationInput& input,
                         const ExecutionContext& context);
MeshInspection Inspect(const MaterializedMesh& source,
                       std::uint64_t issue_limit);
void ExactIngress();
void SelectionValues();
void RawDefectsAndLimits();
template <class T>
void RequireBits(std::span<const T> actual, std::span<const T> expected,
                 const char* message) {
  Require(actual.size() == expected.size(), message);
  Require(actual.empty() || std::memcmp(actual.data(), expected.data(),
                                        actual.size_bytes()) == 0,
          message);
}
template <class T, std::size_t N>
void RequireBits(std::span<const T> actual, const std::array<T, N>& expected,
                 const char* message) {
  RequireBits(actual, std::span<const T>(expected), message);
}
template <class T>
AttributeMaterializationInput Channel(AttributeDomain domain,
                                      std::string_view name,
                                      std::span<const T> scalars,
                                      std::uint32_t components = 1) {
  AttributeMaterializationInput result;
  result.domain = domain;
  result.name = name;
  result.components = components;
  result.values = scalars;
  return result;
}
}  // namespace materialization_test
#endif
