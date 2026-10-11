// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/mesh.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <variant>

#include "fixture_support.h"
namespace materialization_test {
namespace {
constexpr std::size_t kScalarEncodingCount = 7;
constexpr std::size_t kSignedIngressCount = 2;
constexpr std::uint32_t kSingleComponent = 1;
constexpr std::uint64_t kInspectionIssueCap = 64;
template <class T, std::size_t N>
std::span<const T> Read(const std::array<T, N>& values) {
  return values;
}
void ScalarEncodings() {
  const std::array<double, 12> positions{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  const std::array<index_t, 2> offsets{0, 4};
  const std::array<index_t, 4> corners{0, 1, 2, 3};
  const std::array<float, 4> f32{std::bit_cast<float>(0x80000000U),
                                 std::bit_cast<float>(0x7fc12345U),
                                 std::bit_cast<float>(0xff812345U), 1.25F};
  const std::array<double, 4> f64{std::bit_cast<double>(0x8000000000000000ULL),
                                  std::bit_cast<double>(0x7ff8123456789abcULL),
                                  std::bit_cast<double>(0xfff0123456789abcULL),
                                  1.25};
  const std::array<std::int32_t, 4> i32{
      std::numeric_limits<std::int32_t>::min(),
      std::numeric_limits<std::int32_t>::max(), -42, 42};
  const std::array<std::uint8_t, 4> u8{0, 255, 1, 128};
  const std::array<std::uint16_t, 4> u16{0, 65535, 1, 32768};
  const std::array<std::uint32_t, 4> u32{0, 0xffffffffU, 1, 0x80000000U};
  const std::array<std::uint64_t, 4> u64{
      0, 0xffffffffffffffffULL, 0x0020000000000001ULL, 0x8000000000000000ULL};
  const std::array<std::int8_t, 4> i8{-128, -1, 0, 127};
  const std::array<std::int16_t, 4> i16{-32768, -1, 0, 32767};
  const std::array<std::int32_t, 4> i8_expected{-128, -1, 0, 127};
  const std::array<std::int32_t, 4> i16_expected{-32768, -1, 0, 32767};
  std::array<AttributeMaterializationInput,
             kScalarEncodingCount + kSignedIngressCount>
      attributes{Channel(AttributeDomain::vertex, "f32", Read(f32)),
                 Channel(AttributeDomain::vertex, "f64", Read(f64)),
                 Channel(AttributeDomain::vertex, "i32", Read(i32)),
                 Channel(AttributeDomain::vertex, "u8", Read(u8)),
                 Channel(AttributeDomain::vertex, "u16", Read(u16)),
                 Channel(AttributeDomain::vertex, "u32", Read(u32)),
                 Channel(AttributeDomain::vertex, "u64", Read(u64)),
                 Channel(AttributeDomain::vertex, "i8", Read(i8)),
                 Channel(AttributeDomain::vertex, "i16", Read(i16))};
  const MeshMaterializationInput input{
      Read(positions), PolygonTopologyInput{offsets, corners}, attributes};
  ExecutionContext context;
  const auto captured = Capture(input, context);
  const auto view = captured.View();
  Require(view.attributes.size() == attributes.size(),
          "canonical encoding count/source order");
  RequireBits(std::get<std::span<const float>>(view.attributes[0].values), f32,
              "f32 exact bits");
  RequireBits(std::get<std::span<const double>>(view.attributes[1].values), f64,
              "f64 exact bits");
  RequireBits(
      std::get<std::span<const std::int32_t>>(view.attributes[2].values), i32,
      "i32 exact codes");
  RequireBits(
      std::get<std::span<const std::uint8_t>>(view.attributes[3].values), u8,
      "u8 exact codes");
  RequireBits(
      std::get<std::span<const std::uint16_t>>(view.attributes[4].values), u16,
      "u16 exact codes");
  RequireBits(
      std::get<std::span<const std::uint32_t>>(view.attributes[5].values), u32,
      "u32 exact codes");
  RequireBits(
      std::get<std::span<const std::uint64_t>>(view.attributes[6].values), u64,
      "u64 above2^53 codes");
  RequireBits(
      std::get<std::span<const std::int32_t>>(view.attributes[7].values),
      i8_expected, "signed8 widening");
  RequireBits(
      std::get<std::span<const std::int32_t>>(view.attributes[8].values),
      i16_expected, "signed16 widening");
  for (std::size_t i = 0; i < attributes.size(); ++i)
    Require(view.attributes[i].name == attributes[i].name &&
                view.attributes[i].components == kSingleComponent,
            "descriptor identity/source order changed");
  Require(Inspect(captured, kInspectionIssueCap).IsStorageValid(),
          "attribute NaNs rejected by storage inspection");
}
void PositionWidening() {
  // Literal IEEE754 expectations are independent of the producer's widening
  // helper.
  const std::array<std::uint32_t, 12> input_bits{
      0x00000000U, 0x80000000U, 0x3f800000U, 0x00000001U,
      0x007fffffU, 0x00800000U, 0x7f7fffffU, 0x7f800000U,
      0xff800000U, 0x7fc12345U, 0xff812345U, 0xbf800000U};
  const std::array<std::uint64_t, 12> expected_bits{
      0x0000000000000000ULL, 0x8000000000000000ULL, 0x3ff0000000000000ULL,
      0x36a0000000000000ULL, 0x380fffffc0000000ULL, 0x3810000000000000ULL,
      0x47efffffe0000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
      0x7ff82468a0000000ULL, 0xfff02468a0000000ULL, 0xbff0000000000000ULL};
  std::array<float, 12> positions{};
  for (std::size_t i = 0; i < positions.size(); ++i)
    positions[i] = std::bit_cast<float>(input_bits[i]);
  const std::array<index_t, 1> offsets{0};
  const MeshMaterializationInput input{
      Read(positions), PolygonTopologyInput{offsets, {}}, {}};
  ExecutionContext context;
  const auto captured = Capture(input, context);
  positions.fill(42.0F);
  const auto copied = captured;
  const auto actual = copied.View().positions;
  Require(actual.size() == expected_bits.size(), "widened XYZ extent");
  for (std::size_t i = 0; i < actual.size(); ++i)
    Require(std::bit_cast<std::uint64_t>(actual[i]) == expected_bits[i],
            "literal XYZ widening bit pattern");
  Require(copied.View().positions.data() == captured.View().positions.data(),
          "immutable handle copy did not share storage");
}
void DoublePositions() {
  const std::array<std::uint64_t, 9> bits{
      0x8000000000000000ULL, 0x0000000000000001ULL, 0x7ff0000000000000ULL,
      0xfff0000000000000ULL, 0x7ff8123456789abcULL, 0xfff0123456789abcULL,
      0x0010000000000000ULL, 0x7fefffffffffffffULL, 0xbff0000000000000ULL};
  std::array<double, 9> positions{};
  for (std::size_t i = 0; i < bits.size(); ++i)
    positions[i] = std::bit_cast<double>(bits[i]);
  const std::array<index_t, 1> offsets{0};
  const MeshMaterializationInput input{
      Read(positions), PolygonTopologyInput{offsets, {}}, {}};
  ExecutionContext context;
  const auto source = Capture(input, context);
  for (std::size_t i = 0; i < bits.size(); ++i)
    Require(std::bit_cast<std::uint64_t>(source.View().positions[i]) == bits[i],
            "binary64 XYZ raw bits changed");
}
}  // namespace
void ExactIngress() {
  ScalarEncodings();
  PositionWidening();
  DoublePositions();
}
}  // namespace materialization_test
