// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/mesh.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace meshvale::geometry;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
bool has(const std::vector<Diagnostic>& issues, const std::string& code) {
  return std::any_of(issues.begin(), issues.end(),
                     [&](const auto& d) { return d.code == code; });
}
Mesh mixed_mesh() {
  Mesh mesh;
  mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 0, 0}, {2, 1, 0},
                    {4, 0, 0}, {6, 0, 0}, {6, 2, 0}, {5, 1, 0}, {4, 2, 0}};
  mesh.face_offsets = {0, 3, 7, 12};
  mesh.corner_vertices = {0, 1, 2, 1, 3, 4, 2, 5, 6, 7, 8, 9};
  return mesh;
}
Attribute uv(std::string name, std::uint32_t set, float first) {
  Attribute attribute;
  attribute.domain = AttributeDomain::corner;
  attribute.name = std::move(name);
  attribute.semantic = "texcoord";
  attribute.set_index = set;
  attribute.components = 2;
  attribute.values =
      std::vector<float>{first, 0, 1, 0, 0, 1, 0.25F, 0, 1,    0,    1, 1,
                         0.25F, 1, 0, 0, 1, 0, 1,     1, 0.5F, 0.5F, 0, 1};
  return attribute;
}
void multiple_uvs_and_domains() {
  auto mesh = mixed_mesh();
  mesh.attributes = {uv("uv0", 0, 0), uv("lightmap", 1, 0.5F)};
  Attribute vertex_color;
  vertex_color.name = "uv0";  // Same name in another domain is unambiguous.
  vertex_color.semantic = "custom";
  vertex_color.values = std::vector<std::uint8_t>(10, 7);
  mesh.attributes.push_back(vertex_color);
  const auto loops = mesh.corner_vertices;
  require(inspect_storage(mesh).empty(),
          "mixed mesh with two UV maps rejected");
  require(mesh.corner_vertices == loops && mesh.face_count() == 3,
          "inspection changed polygons");
  const auto& first = std::get<std::vector<float>>(mesh.attributes[0].values);
  const auto& second = std::get<std::vector<float>>(mesh.attributes[1].values);
  require(first[0] != second[0], "UV sets aliased");
  require(first[2] != first[6], "corner seam collapsed at shared vertex");
  mesh.attributes.push_back(mesh.attributes[0]);
  require(has(inspect_storage(mesh), "attribute.duplicate_name"),
          "duplicate domain/name accepted");
}
void flexible_skinning() {
  auto mesh = mixed_mesh();
  Attribute joints;
  joints.name = "joints";
  joints.semantic = "joint_indices";
  joints.offsets = std::vector<index_t>{0, 0, 2, 7, 7, 7, 7, 7, 7, 7, 7};
  joints.values = std::vector<std::uint32_t>{1, 2, 0, 1, 2, 3, 4};
  joints.metadata["association"] = "body_skin";
  Attribute weights = joints;
  weights.name = "weights";
  weights.semantic = "joint_weights";
  weights.values = std::vector<double>{0.25, 0.75, 0.1, 0.2, 0.3, 0.4, 0.5};
  mesh.attributes = {joints, weights};
  require(inspect_storage(mesh).empty(),
          "zero/variable/five influences rejected");
  require(
      std::get<std::vector<double>>(mesh.attributes[1].values).back() == 0.5,
      "storage normalized or truncated weights");
  mesh.attributes[1].offsets->at(3) = 6;
  require(inspect_storage(mesh).empty(),
          "structural inspector imposed skin-pair semantics");
  // The asset/operation semantic checker must validate paired row lengths
  // separately.
}
void missing_rows_and_bad_storage() {
  auto mesh = mixed_mesh();
  auto attr = uv("uv0", 0, 0);
  attr.present = std::vector<std::uint8_t>(12, 1);
  attr.present->at(0) = 0;
  mesh.attributes.push_back(attr);
  require(inspect_storage(mesh).empty(), "missing value cannot be represented");
  require(mesh.attributes[0].present->at(0) == 0,
          "missing value silently authored");
  mesh.attributes[0].present->at(0) = 2;
  require(has(inspect_storage(mesh), "attribute.presence_value"),
          "bad presence value accepted");
  mesh.attributes[0].present->pop_back();
  require(has(inspect_storage(mesh), "attribute.presence_count"),
          "short presence mask accepted");
  mesh.attributes[0].components = 0;
  require(has(inspect_storage(mesh), "attribute.zero_components"),
          "zero components accepted");
  mesh.attributes[0].components = 3;
  require(has(inspect_storage(mesh), "attribute.row_count"),
          "bad dense cardinality accepted");
  mesh.attributes[0].offsets = std::vector<index_t>{};
  require(has(inspect_storage(mesh), "attribute.empty_offsets"),
          "empty ragged offsets accepted");
  mesh.attributes[0].offsets = std::vector<index_t>{1, 6, 3, 99};
  auto issues = inspect_storage(mesh);
  require(has(issues, "attribute.offset_start") &&
              has(issues, "attribute.offset_order") &&
              has(issues, "attribute.offset_range") &&
              has(issues, "attribute.offset_end"),
          "malformed ragged offsets missed");
  mesh.attributes[0].offsets = std::vector<index_t>{0, 1, 24};
  require(has(inspect_storage(mesh), "attribute.component_alignment"),
          "split tuple accepted");
}
void raw_defects() {
  auto mesh = mixed_mesh();
  mesh.corner_vertices[0] = 99;
  require(has(inspect_storage(mesh), "mesh.vertex_range"),
          "invalid vertex index missed");
  require(mesh.corner_vertices[0] == 99,
          "inspection normalized malformed data");
  mesh.face_offsets = {0, 3, 2, 99};
  auto issues = inspect_storage(mesh);
  require(has(issues, "mesh.offset_order") && has(issues, "mesh.offset_range"),
          "invalid face offsets missed");
  mesh.face_offsets = {};
  require(has(inspect_storage(mesh), "mesh.empty_offsets"),
          "empty face offsets missed");
  mesh = mixed_mesh();
  mesh.positions.Set(0, {std::numeric_limits<double>::infinity(), 0, 0});
  require(has(inspect_storage(mesh), "mesh.nonfinite_position"),
          "nonfinite position missed");
  require(inspect_storage(Mesh{}).empty(), "empty mesh rejected");
}
void PositionOwnershipAndBytes() {
  const std::array<std::uint64_t, 6> bits{
      UINT64_C(0x8000000000000000), UINT64_C(0x7ff8123456789abc),
      UINT64_C(0x7ff0123456789abc), UINT64_C(0x3ff0000000000000),
      UINT64_C(0x4000000000000000), UINT64_C(0x4008000000000000)};
  std::array<std::byte, sizeof(bits) + 1> misaligned{};
  std::memcpy(misaligned.data() + 1, bits.data(), sizeof(bits));
  PositionBuffer positions;
  positions.AssignBytes({misaligned.data() + 1, sizeof(bits)});
  auto copy = positions;
  positions.Set(1, {9, 8, 7});
  auto moved = std::move(copy);
  require(copy.empty() && moved.size() == 2 && moved.Get(1)[2] == 3,
          "position copies/moves borrowed or changed rows");
  std::array<std::uint64_t, 6> exported{};
  moved.CopyBytesTo(std::as_writable_bytes(std::span(exported)));
  require(exported == bits, "position byte transfer lost NaN/signed-zero bits");
  moved.reserve(16);
  require(moved.size() == 2, "reserve exposed uninitialized capacity rows");
  moved.CopyBytesTo(std::as_writable_bytes(std::span(exported)));
  require(exported == bits, "position growth changed scalar bits");
  auto row = moved.Get(0);
  row[0] = 7;
  require(std::bit_cast<std::uint64_t>(moved.Get(0)[0]) == bits[0],
          "Get returned a borrowed position row");
  bool rejected = false;
  try {
    (void)moved.Get(2);
  } catch (const std::out_of_range&) {
    rejected = true;
  }
  require(rejected, "invalid Get index accepted");
  rejected = false;
  try {
    moved.Set(2, row);
  } catch (const std::out_of_range&) {
    rejected = true;
  }
  require(rejected, "invalid Set index accepted");
  rejected = false;
  try {
    moved.AssignBytes({misaligned.data(), 1});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected && moved.size() == 2, "bad byte shape changed positions");
  rejected = false;
  try {
    moved.CopyBytesTo({misaligned.data(), 1});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "bad destination extent accepted");
  rejected = false;
  try {
    moved.reserve(std::numeric_limits<std::size_t>::max());
  } catch (const std::length_error&) {
    rejected = true;
  }
  require(rejected && moved.size() == 2, "shape overflow did not roll back");
  moved.CopyBytesTo(std::as_writable_bytes(std::span(exported)));
  require(exported == bits, "failed mutation changed raw bytes");
  moved = std::move(moved);
  moved = moved;
  require(moved.size() == 2, "self assignment lost ownership");
  moved.clear();
  moved.Append({4, 5, 6});
  require(moved.size() == 1 && moved.Get(0)[0] == 4,
          "clear/reuse exposed old rows");
  PositionBuffer empty;
  empty.AssignBytes({});
  empty.CopyBytesTo({});
  require(empty.empty(), "empty byte transfers changed shape");
}
int main() {
  try {
    multiple_uvs_and_domains();
    flexible_skinning();
    missing_rows_and_bad_storage();
    raw_defects();
    PositionOwnershipAndBytes();
    std::cout << "Five storage suites passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
