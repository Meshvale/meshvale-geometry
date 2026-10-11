// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/mesh.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <variant>

#include "fixture_support.h"
namespace materialization_test {
namespace {
constexpr std::uint64_t kInspectionIssueCap = 64;
constexpr std::uint32_t kPrimaryUvSet = 0;
constexpr std::uint32_t kSecondaryUvSet = 2;
constexpr std::size_t kJointsAttribute = 0;
constexpr std::size_t kWeightsAttribute = 1;
constexpr std::size_t kPrimaryUvAttribute = 2;
constexpr std::size_t kSecondaryUvAttribute = 3;
constexpr std::size_t kNormalsAttribute = 4;
constexpr std::size_t kMaterialAttribute = 5;
constexpr std::size_t kFaceRaggedAttribute = 6;
constexpr std::size_t kCornerRaggedAttribute = 7;
template <class T, std::size_t N>
std::span<const T> Read(const std::array<T, N>& values) {
  return values;
}
void DescriptorsEqual(const AttributeReadView& actual,
                      const AttributeReadView& source) {
  Require(actual.domain == source.domain && actual.name == source.name &&
              actual.semantic == source.semantic &&
              actual.set_index == source.set_index &&
              actual.components == source.components,
          "selected descriptor changed");
  Require(actual.metadata.size() == source.metadata.size(),
          "metadata entry count changed");
  for (std::size_t i = 0; i < actual.metadata.size(); ++i)
    Require(actual.metadata[i].key == source.metadata[i].key &&
                actual.metadata[i].value == source.metadata[i].value,
            "metadata byte/order retention");
}
}  // namespace
void SelectionValues() {
  // Vertex5 remains unused even before selection. The first triangle is
  // omitted.
  const std::array<double, 18> positions{0, 0, 0, 1, 0, 0, 1,  1,  0,
                                         2, 0, 0, 2, 1, 0, 99, 99, 99};
  const std::array<index_t, 3> offsets{0, 3, 7};
  const std::array<index_t, 7> corners{0, 1, 2, 1, 3, 4, 2};
  const std::array<std::uint64_t, 7> joints{
      0x0020000000000001ULL, 2, 3, 4, 5, 6, 7};
  const std::array<float, 7> weights{0.25F, 0.5F, 0.75F, 2.0F,
                                     -0.5F, 0.0F, 3.0F};
  const std::array<index_t, 7> influence_offsets{0, 1, 1, 4, 5, 5, 7};
  const std::array<std::uint8_t, 6> vertex_present{1, 0, 1, 1, 0, 1};
  const std::array<float, 14> primary_uv{0, 1, 2, 3,  4,  5,  6,
                                         7, 8, 9, 10, 11, 12, 13};
  const std::array<float, 14> secondary_uv{20, 21, 22, 23, 24, 25, 26,
                                           27, 28, 29, 30, 31, 32, 33};
  const std::array<double, 21> normals{1,  2,  3,  4,  5,  6,  7,
                                       8,  9,  10, 11, 12, 13, 14,
                                       15, 16, 17, 18, 19, 20, 21};
  const std::array<std::uint16_t, 2> materials{9, 11};
  const std::array<std::uint8_t, 2> face_present{1, 0};
  const std::array<std::int32_t, 3> face_values{31, 32, 33};
  const std::array<index_t, 3> face_rows{0, 0, 3};
  const std::array<std::uint32_t, 8> corner_values{20, 21, 22, 23,
                                                   24, 25, 26, 27};
  const std::array<index_t, 8> corner_rows{0, 1, 3, 3, 4, 7, 7, 8};
  const std::array<std::uint8_t, 7> corner_present{1, 0, 1, 1, 0, 1, 1};
  constexpr std::string_view kEmbeddedName{"uv\0set", 6};
  constexpr std::string_view kEmbeddedSemantic{"TEX\0COORD", 9};
  const std::array<MetadataEntryView, 2> metadata{
      {{"z", "last"},
       {std::string_view{"a\0key", 5}, std::string_view{"v\0alue", 6}}}};
  std::array<AttributeMaterializationInput, 8> attributes{
      Channel(AttributeDomain::vertex, "joints", Read(joints)),
      Channel(AttributeDomain::vertex, "weights", Read(weights)),
      Channel(AttributeDomain::corner, kEmbeddedName, Read(primary_uv), 2),
      Channel(AttributeDomain::corner, "uv2", Read(secondary_uv), 2),
      Channel(AttributeDomain::corner, "normals", Read(normals), 3),
      Channel(AttributeDomain::face, "materials", Read(materials)),
      Channel(AttributeDomain::face, "custom-face", Read(face_values)),
      Channel(AttributeDomain::corner, "custom-corner", Read(corner_values))};
  attributes[kJointsAttribute].offsets = Read(influence_offsets);
  attributes[kJointsAttribute].present = Read(vertex_present);
  attributes[kWeightsAttribute].offsets = Read(influence_offsets);
  attributes[kWeightsAttribute].present = Read(vertex_present);
  attributes[kPrimaryUvAttribute].semantic = kEmbeddedSemantic;
  attributes[kPrimaryUvAttribute].set_index = kPrimaryUvSet;
  attributes[kSecondaryUvAttribute].set_index = kSecondaryUvSet;
  attributes[kMaterialAttribute].present = Read(face_present);
  attributes[kFaceRaggedAttribute].offsets = Read(face_rows);
  attributes[kFaceRaggedAttribute].present = Read(face_present);
  attributes[kCornerRaggedAttribute].offsets = Read(corner_rows);
  attributes[kCornerRaggedAttribute].present = Read(corner_present);
  for (auto& attribute : attributes) attribute.metadata = metadata;
  const MeshMaterializationInput input{
      Read(positions), PolygonTopologyInput{offsets, corners}, attributes};
  ExecutionContext context;
  const auto source = Capture(input, context);
  Require(Inspect(source, kInspectionIssueCap).IsStorageValid(),
          "valid mixed-domain source rejected");
  const auto source_view = source.View();
  for (std::size_t i = 0; i < attributes.size(); ++i) {
    const auto& actual = source_view.attributes[i];
    const auto& expected = attributes[i];
    Require(actual.domain == expected.domain && actual.name == expected.name &&
                actual.semantic == expected.semantic &&
                actual.set_index == expected.set_index &&
                actual.components == expected.components,
            "captured descriptor bytes/source order");
    Require(actual.metadata.size() == metadata.size(),
            "captured metadata count");
    for (std::size_t entry = 0; entry < metadata.size(); ++entry)
      Require(actual.metadata[entry].key == metadata[entry].key &&
                  actual.metadata[entry].value == metadata[entry].value,
              "captured metadata bytes/order");
  }
  const std::array<index_t, 1> kept{1};
  auto result = SelectMeshFaces(source, kept, Limits(), Work());
  Require(result.status == MaterializationStatus::kSuccess &&
              result.selection.has_value(),
          "quad face selection failed");
  const auto& selected = *result.selection;
  const auto candidate = selected.Candidate().View();
  const std::array<index_t, 2> expected_offsets{0, 4};
  const std::array<index_t, 4> expected_corners{1, 3, 4, 2};
  const std::array<index_t, 4> source_corners{3, 4, 5, 6};
  RequireBits(candidate.positions, positions,
              "unused source vertices removed or altered");
  Require(candidate.positions.data() == source_view.positions.data(),
          "unchanged vertex position part not shared");
  RequireBits(candidate.face_offsets, expected_offsets,
              "quad triangulated or wrong offsets");
  RequireBits(candidate.corner_vertices, expected_corners,
              "quad winding/loop order changed");
  RequireBits(selected.SourceFaces(), kept, "output-to-source face map");
  RequireBits(selected.SourceCorners(), source_corners,
              "output-to-source corner map");
  Require(selected.SourceVertexCount() == 6 &&
              selected.SourceFaceCount() == 2 &&
              selected.SourceCornerCount() == 7,
          "source physical counts lost");
  Require(candidate.attributes.size() == attributes.size(),
          "selected channels omitted");
  for (std::size_t i = 0; i < attributes.size(); ++i)
    DescriptorsEqual(candidate.attributes[i], source_view.attributes[i]);
  for (const auto index : {kJointsAttribute, kWeightsAttribute}) {
    Require(candidate.attributes[index].offsets.has_value() &&
                candidate.attributes[index].present.has_value(),
            "shared vertex row metadata omitted");
    RequireBits(*candidate.attributes[index].offsets, influence_offsets,
                "arbitrary vertex influences changed");
    RequireBits(*candidate.attributes[index].present, vertex_present,
                "vertex missingness changed");
  }
  const auto selected_joints = std::get<std::span<const std::uint64_t>>(
      candidate.attributes[kJointsAttribute].values);
  const auto selected_weights = std::get<std::span<const float>>(
      candidate.attributes[kWeightsAttribute].values);
  RequireBits(selected_joints, joints, "joint codes truncated");
  RequireBits(selected_weights, weights, "weights normalized/truncated");
  Require(selected_joints.data() ==
                  std::get<std::span<const std::uint64_t>>(
                      source_view.attributes[kJointsAttribute].values)
                      .data() &&
              selected_weights.data() ==
                  std::get<std::span<const float>>(
                      source_view.attributes[kWeightsAttribute].values)
                      .data(),
          "vertex channels not shared");
  const std::array<float, 8> expected_primary_uv{6, 7, 8, 9, 10, 11, 12, 13};
  const std::array<float, 8> expected_secondary_uv{26, 27, 28, 29,
                                                   30, 31, 32, 33};
  const std::array<double, 12> expected_normals{10, 11, 12, 13, 14, 15,
                                                16, 17, 18, 19, 20, 21};
  const std::array<std::uint16_t, 1> expected_material{11};
  const std::array<std::uint8_t, 1> expected_face_present{0};
  const std::array<index_t, 2> expected_face_rows{0, 3};
  const std::array<std::uint32_t, 5> expected_corner_values{23, 24, 25, 26, 27};
  const std::array<index_t, 5> expected_corner_rows{0, 1, 4, 4, 5};
  const std::array<std::uint8_t, 4> expected_corner_present{1, 0, 1, 1};
  RequireBits(std::get<std::span<const float>>(
                  candidate.attributes[kPrimaryUvAttribute].values),
              expected_primary_uv, "UV seam/source rows changed");
  RequireBits(std::get<std::span<const float>>(
                  candidate.attributes[kSecondaryUvAttribute].values),
              expected_secondary_uv, "second UV set changed");
  RequireBits(std::get<std::span<const double>>(
                  candidate.attributes[kNormalsAttribute].values),
              expected_normals, "normals regenerated or converted");
  RequireBits(std::get<std::span<const std::uint16_t>>(
                  candidate.attributes[kMaterialAttribute].values),
              expected_material, "missing material backing dropped");
  RequireBits(*candidate.attributes[kMaterialAttribute].present,
              expected_face_present, "face missingness changed");
  RequireBits(std::get<std::span<const std::int32_t>>(
                  candidate.attributes[kFaceRaggedAttribute].values),
              face_values, "face custom ragged values changed");
  RequireBits(*candidate.attributes[kFaceRaggedAttribute].offsets,
              expected_face_rows, "face ragged offsets wrong");
  RequireBits(std::get<std::span<const std::uint32_t>>(
                  candidate.attributes[kCornerRaggedAttribute].values),
              expected_corner_values, "corner ragged backing changed");
  RequireBits(*candidate.attributes[kCornerRaggedAttribute].offsets,
              expected_corner_rows, "corner ragged row map wrong");
  RequireBits(*candidate.attributes[kCornerRaggedAttribute].present,
              expected_corner_present, "corner missingness wrong");
  Require(Inspect(selected.Candidate(), kInspectionIssueCap).IsStorageValid(),
          "selection produced invalid storage");
  auto empty = SelectMeshFaces(source, {}, Limits(), Work());
  Require(empty.status == MaterializationStatus::kSuccess &&
              empty.selection.has_value(),
          "empty selection rejected");
  const auto empty_view = empty.selection->Candidate().View();
  const std::array<index_t, 1> empty_offsets{0};
  RequireBits(empty_view.face_offsets, empty_offsets,
              "empty selection canonical offset");
  Require(empty_view.corner_vertices.empty() &&
              empty.selection->SourceFaces().empty() &&
              empty.selection->SourceCorners().empty(),
          "empty selection maps not empty");
  Require(empty_view.positions.data() == source_view.positions.data() &&
              empty_view.attributes.size() == attributes.size(),
          "empty selection discarded vertices/descriptors");
  for (std::size_t i = kPrimaryUvAttribute; i < attributes.size(); ++i) {
    Require(std::visit([](auto values) { return values.empty(); },
                       empty_view.attributes[i].values),
            "empty selection kept face/corner scalar rows");
    DescriptorsEqual(empty_view.attributes[i], source_view.attributes[i]);
  }
  for (const auto index : {kFaceRaggedAttribute, kCornerRaggedAttribute}) {
    Require(empty_view.attributes[index].offsets.has_value(),
            "empty selected ragged offsets absent");
    RequireBits(*empty_view.attributes[index].offsets, empty_offsets,
                "empty selected ragged offset must be zero");
  }
  for (const auto index :
       {kMaterialAttribute, kFaceRaggedAttribute, kCornerRaggedAttribute})
    Require(empty_view.attributes[index].present.has_value() &&
                empty_view.attributes[index].present->empty(),
            "empty selected authored presence must remain present");
  for (const auto index :
       {kPrimaryUvAttribute, kSecondaryUvAttribute, kNormalsAttribute})
    Require(!empty_view.attributes[index].offsets &&
                !empty_view.attributes[index].present,
            "empty selected dense absent row arrays became authored");
  const std::array<index_t, 2> repeated{1, 1};
  const std::array<index_t, 2> reversed{1, 0};
  const std::array<index_t, 1> outside{2};
  for (const auto selection : {Read(repeated), Read(reversed)}) {
    const auto rejected = SelectMeshFaces(source, selection, Limits(), Work());
    Require(rejected.status == MaterializationStatus::kInvalidSelection &&
                !rejected.selection &&
                rejected.diagnostic.code ==
                    MaterializationDiagnosticCode::kSelectionOrder,
            "unordered/repeated selection accepted");
  }
  const auto rejected = SelectMeshFaces(source, outside, Limits(), Work());
  Require(rejected.status == MaterializationStatus::kInvalidSelection &&
              !rejected.selection &&
              rejected.diagnostic.code ==
                  MaterializationDiagnosticCode::kSelectionRange,
          "out-of-range selection accepted");
}
}  // namespace materialization_test
