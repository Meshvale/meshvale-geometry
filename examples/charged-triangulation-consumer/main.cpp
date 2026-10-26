// SPDX-License-Identifier: Apache-2.0

#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/charged_triangulation.h>
#include <meshvale/geometry/editable_mesh.h>
#include <meshvale/geometry/execution.h>
#include <meshvale/geometry/materialization.h>
#include <meshvale/geometry/mesh.h>
#include <meshvale/geometry/position_buffer.h>
#include <meshvale/geometry/scalar_buffer.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace meshvale::geometry::charged_example {
inline constexpr std::size_t kPositionComponents = 3;
inline constexpr std::size_t kTriangleCorners = 3;
inline constexpr std::size_t kFixtureVertices = 12;
inline constexpr std::size_t kFixtureFaces = 3;
inline constexpr std::size_t kFixtureCorners = 12;
inline constexpr std::size_t kFixtureAttributes = 19;
inline constexpr std::size_t kOutputFaces = 6;
inline constexpr std::size_t kOutputCorners = 18;
inline constexpr std::uint64_t kMiB = 1024 * 1024;
inline constexpr std::uint64_t kCountLimit = 1024 * 1024;
inline constexpr std::uint64_t kRequestLimit = 64 * kMiB;
inline constexpr std::size_t kLedgerBytes = 256 * kMiB;
inline constexpr std::size_t kWorkers = 4;
inline constexpr std::size_t kBitsPerByte = 8;
inline constexpr std::size_t kBitsPerNibble = 4;
inline constexpr std::size_t kHexCharactersPerByte = 2;
inline constexpr std::uint8_t kNibbleMask = 15;
inline constexpr std::string_view kHexDigits = "0123456789abcdef";
inline void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
inline std::uint8_t Nibble(char value) {
  const auto found = kHexDigits.find(value);
  Require(found != std::string_view::npos, "fixture hex digit");
  return static_cast<std::uint8_t>(found);
}
inline std::string Bytes(std::string_view hex) {
  Require(hex.size() % kHexCharactersPerByte == 0, "fixture byte extent");
  std::string value;
  value.reserve(hex.size() / kHexCharactersPerByte);
  for (std::size_t i = 0; i < hex.size(); i += kHexCharactersPerByte)
    value.push_back(static_cast<char>((Nibble(hex[i]) << kBitsPerNibble) |
                                      Nibble(hex[i + 1])));
  return value;
}
template <class T>
std::uint64_t Bits(T value) {
  if constexpr (std::is_same_v<T, float> || std::is_same_v<T, std::int32_t>)
    return std::bit_cast<std::uint32_t>(value);
  else if constexpr (std::is_same_v<T, double>)
    return std::bit_cast<std::uint64_t>(value);
  else
    return value;
}
template <class T>
T FromBits(std::uint64_t value) {
  if constexpr (std::is_same_v<T, float> || std::is_same_v<T, std::int32_t>)
    return std::bit_cast<T>(static_cast<std::uint32_t>(value));
  else if constexpr (std::is_same_v<T, double>)
    return std::bit_cast<double>(value);
  else
    return static_cast<T>(value);
}
template <class T>
ScalarBuffer<T> Values(std::string_view hex) {
  Require(hex.size() % (kHexCharactersPerByte * sizeof(T)) == 0,
          "fixture scalar extent");
  ScalarBuffer<T> output;
  for (std::size_t start = 0; start < hex.size();
       start += kHexCharactersPerByte * sizeof(T)) {
    std::uint64_t word = 0;
    for (std::size_t byte = 0; byte < sizeof(T); ++byte)
      word |= static_cast<std::uint64_t>(
                  (Nibble(hex[start + kHexCharactersPerByte * byte])
                   << kBitsPerNibble) |
                  Nibble(hex[start + kHexCharactersPerByte * byte + 1]))
              << (kBitsPerByte * byte);
    output.push_back(FromBits<T>(word));
  }
  return output;
}
inline MeshMaterializationLimits MeshLimits() {
  return {kCountLimit, kCountLimit, kCountLimit,  kCountLimit,
          kCountLimit, kCountLimit, kCountLimit,  kCountLimit,
          kCountLimit, kCountLimit, kRequestLimit};
}
inline ChargedTriangulationLimits Limits() {
  return {MeshLimits(), {kCountLimit, kRequestLimit},
          MeshLimits(), {kCountLimit, kRequestLimit},
          kCountLimit,  kRequestLimit,
          kRequestLimit};
}
inline MeshMaterializationWorkPolicy Work() { return {1, 64, 16, 8}; }
// Original oblique dyadic fixture: three mixed faces, all seven dense/ragged
// types, missing backing, normal/UV seams, distinct coincident IDs and unused
// vertex. Hex strings encode complete raw words, not normalized numbers.
inline Mesh MakeLegacy() {
  Mesh mesh;
  constexpr std::array<std::array<std::uint64_t, kPositionComponents>,
                       kFixtureVertices>
      positions{{
          {{0x3fc0000000000000ULL, 0xbfd0000000000000ULL,
            0x3fd8000000000000ULL}},
          {{0x3ff2000000000000ULL, 0x3fd0000000000000ULL,
            0x3fc0000000000000ULL}},
          {{0xbfd8000000000000ULL, 0x3fe8000000000000ULL,
            0x3ff2000000000000ULL}},
          {{0xbfec000000000000ULL, 0x3ffc000000000000ULL,
            0x3ffe000000000000ULL}},
          {{0x3fc0000000000000ULL, 0x4002000000000000ULL,
            0x3ffa000000000000ULL}},
          {{0x3fe4000000000000ULL, 0x3ff4000000000000ULL,
            0x3fec000000000000ULL}},
          {{0xbfd8000000000000ULL, 0x3fe8000000000000ULL,
            0x3ff2000000000000ULL}},
          {{0x3ff2000000000000ULL, 0x3ff8000000000000ULL,
            0x3fe8000000000000ULL}},
          {{0x3fd8000000000000ULL, 0x4008000000000000ULL,
            0x3ffe000000000000ULL}},
          {{0xbfc0000000000000ULL, 0x3ff8000000000000ULL,
            0x3ff6000000000000ULL}},
          {{0xbff2000000000000ULL, 0x4002000000000000ULL,
            0x4002000000000000ULL}},
          {{0x0000000000000000ULL, 0x8000000000000000ULL,
            0x3fe2000000000000ULL}},
      }};
  for (const auto& row : positions)
    mesh.positions.Append({std::bit_cast<double>(row[0]),
                           std::bit_cast<double>(row[1]),
                           std::bit_cast<double>(row[2])});
  mesh.face_offsets = {0, 3, 7, 12};
  mesh.corner_vertices = {0, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  {
    Attribute a;
    a.domain = AttributeDomain::vertex;
    a.name = Bytes("64656e73655f666c6f61743332");
    a.semantic = Bytes("4f5041515545");
    a.components = 2;
    a.values = Values<float>(
        "00000000000000804523c17f000080ff0000003f000080bf0000807f8967c5ff010000"
        "00010000800000803e000020c000008040000080c00100803f010080bf000080000000"
        "808000000040000000c000002041000020c10000803d000080bd");
    a.present = std::vector<std::uint8_t>{1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::face;
    a.name = Bytes("64656e73655f666c6f61743634006e616d65");
    a.semantic = Bytes("4f5041515545007061796c6f6164");
    a.components = 2;
    a.values = Values<double>(
        "214300000000f87f0000000000000080000000000000f0ff0000000000000000000000"
        "000000f07f769800000000f8ff");
    a.present = std::vector<std::uint8_t>{1, 0, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::corner;
    a.name = Bytes("64656e73655f696e743332");
    a.semantic = Bytes("4f5041515545");
    a.components = 1;
    a.values = Values<std::int32_t>(
        "00000080ffffff7ff9ffffff000000000b000000edffffff17000000e3ffffff1f0000"
        "00dbffffff29000000d5ffffff");
    a.present = std::vector<std::uint8_t>{1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::vertex;
    a.name = Bytes("64656e73655f75696e7438");
    a.semantic = Bytes("4f5041515545");
    a.components = 1;
    a.values = Values<std::uint8_t>("00ff11408003db085b0207c9");
    a.present = std::vector<std::uint8_t>{1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::face;
    a.name = Bytes("64656e73655f75696e743136");
    a.semantic = Bytes("4f5041515545");
    a.components = 2;
    a.values = Values<std::uint16_t>("0000ffff13000080ff030110");
    a.present = std::vector<std::uint8_t>{1, 0, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::corner;
    a.name = Bytes("64656e73655f75696e743332");
    a.semantic = Bytes("4f5041515545");
    a.components = 1;
    a.values = Values<std::uint32_t>(
        "00000000ffffffff000000800403020111000000010100000100010001efcdab070000"
        "00443322111f000000feffffff");
    a.present = std::vector<std::uint8_t>{1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::vertex;
    a.name = Bytes("64656e73655f75696e743634");
    a.semantic = Bytes("4f5041515545");
    a.components = 1;
    a.values = Values<std::uint64_t>(
        "0000000000000000ffffffffffffffff01000000000000800100000000002000110000"
        "00000000000807060504030201ffffffffffffff7f04030201efbeadde000000000100"
        "0000ffffffff0000000000000000000000806100000000000000");
    a.present = std::vector<std::uint8_t>{1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::corner;
    a.name = Bytes("7261676765645f666c6f61743332");
    a.semantic = Bytes("435553544f4d5f524147474544");
    a.components = 1;
    a.values = Values<float>(
        "8146c27f00000080000000000000807f000080ff0000003e000080be8146c27f000000"
        "80000000000000807f000080ff0000003e000080be8146c27f00000080000000000000"
        "807f000080ff0000003e000080be8146c27f00000080000000000000807f000080ff00"
        "00003e000080be8146c27f00000080");
    a.offsets =
        std::vector<index_t>{0, 0, 2, 7, 9, 9, 14, 16, 21, 21, 23, 28, 30};
    a.present = std::vector<std::uint8_t>{1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::vertex;
    a.name = Bytes("7261676765645f666c6f61743634");
    a.semantic = Bytes("435553544f4d5f524147474544");
    a.components = 1;
    a.values = Values<double>(
        "debc0a000000f87f00000000000000800000000000000000000000000000f07f000000"
        "000000f0ff000000000000c03f000000000000d0bfdebc0a000000f87f000000000000"
        "00800000000000000000000000000000f07f000000000000f0ff000000000000c03f00"
        "0000000000d0bfdebc0a000000f87f0000000000000080000000000000000000000000"
        "0000f07f000000000000f0ff000000000000c03f000000000000d0bfdebc0a000000f8"
        "7f00000000000000800000000000000000000000000000f07f000000000000f0ff0000"
        "00000000c03f000000000000d0bfdebc0a000000f87f0000000000000080");
    a.offsets =
        std::vector<index_t>{0, 0, 2, 7, 9, 9, 14, 16, 21, 21, 23, 28, 30};
    a.present = std::vector<std::uint8_t>{1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::face;
    a.name = Bytes("7261676765645f696e743332");
    a.semantic = Bytes("435553544f4d5f524147474544");
    a.components = 1;
    a.values = Values<std::int32_t>(
        "0000008000000000ffffff7ffbffffff0d000000ebffffff22000000");
    a.offsets = std::vector<index_t>{0, 0, 2, 7};
    a.present = std::vector<std::uint8_t>{1, 0, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::corner;
    a.name = Bytes("7261676765645f75696e7438");
    a.semantic = Bytes("435553544f4d5f524147474544");
    a.components = 1;
    a.values = Values<std::uint8_t>(
        "ff00078013c903ff00078013c903ff00078013c903ff00078013c903ff00");
    a.offsets =
        std::vector<index_t>{0, 0, 2, 7, 9, 9, 14, 16, 21, 21, 23, 28, 30};
    a.present = std::vector<std::uint8_t>{1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::vertex;
    a.name = Bytes("7261676765645f75696e743136");
    a.semantic = Bytes("435553544f4d5f524147474544");
    a.components = 1;
    a.values = Values<std::uint16_t>(
        "ffff000000800500010103100b00ffff000000800500010103100b00ffff0000008005"
        "00010103100b00ffff000000800500010103100b00ffff0000");
    a.offsets =
        std::vector<index_t>{0, 0, 2, 7, 9, 9, 14, 16, 21, 21, 23, 28, 30};
    a.present = std::vector<std::uint8_t>{1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::face;
    a.name = Bytes("7261676765645f75696e743332");
    a.semantic = Bytes("435553544f4d5f524147474544");
    a.components = 1;
    a.values = Values<std::uint32_t>(
        "ffffffff00000000000000800500000078563412feffffff01000100");
    a.offsets = std::vector<index_t>{0, 0, 2, 7};
    a.present = std::vector<std::uint8_t>{1, 0, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::corner;
    a.name = Bytes("7261676765645f75696e743634");
    a.semantic = Bytes("435553544f4d5f524147474544");
    a.components = 1;
    a.values = Values<std::uint64_t>(
        "ffffffffffffffff000000000000000001000000000000800100000000002000050000"
        "000000000088776655443322110df0fecaefbeaddeffffffffffffffff000000000000"
        "000001000000000000800100000000002000050000000000000088776655443322110d"
        "f0fecaefbeaddeffffffffffffffff0000000000000000010000000000008001000000"
        "00002000050000000000000088776655443322110df0fecaefbeaddeffffffffffffff"
        "ff00000000000000000100000000000080010000000000200005000000000000008877"
        "6655443322110df0fecaefbeaddeffffffffffffffff0000000000000000");
    a.offsets =
        std::vector<index_t>{0, 0, 2, 7, 9, 9, 14, 16, 21, 21, 23, 28, 30};
    a.present = std::vector<std::uint8_t>{1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1};
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::corner;
    a.name = Bytes("636f726e65725f6e6f726d616c73");
    a.semantic = Bytes("4e4f524d414c");
    a.components = 3;
    a.values = Values<float>(
        "0000803f000080bf000000400000803f000080bf000000400000803f000080bf000000"
        "40000080bf0000803f000000c0000080bf0000803f000000c0000080bf0000803f0000"
        "00c0000080bf0000803f000000c00000803f000080bf000000400000803f000080bf00"
        "0000400000803f000080bf000000400000803f000080bf000000400000803f000080bf"
        "00000040");
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::corner;
    a.name = Bytes("75765f30");
    a.semantic = Bytes("544558434f4f5244");
    a.components = 2;
    a.set_index = 0;
    a.values = Values<float>(
        "00000000000000000000803f00000000000000000000803f0000803e0000003e000080"
        "3e0000603f0000403f0000603f0000403f0000003e000000bf0000003f0000803f0000"
        "003f0000803f00000040000000000000803f000000bf00000040");
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::corner;
    a.name = Bytes("75765f32");
    a.semantic = Bytes("544558434f4f5244");
    a.components = 2;
    a.set_index = 2;
    a.values = Values<double>(
        "000000000000b03f000000000000c83f000000000000e23f000000000000c83f000000"
        "000000b03f000000000000d4bf000000000000c83f000000000000c03f000000000000"
        "c83f000000000000d0bf000000000000dc3f000000000000d0bf000000000000dc3f00"
        "0000000000c03f000000000000c8bf000000000000b0bf000000000000e23f00000000"
        "0000b0bf000000000000e23f000000000000eabf000000000000b03f000000000000d4"
        "bf000000000000c8bf000000000000eabf");
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::corner;
    a.name = Bytes("75765f35");
    a.semantic = Bytes("544558434f4f5244");
    a.components = 2;
    a.set_index = 5;
    a.values = Values<float>(
        "0000803d0000c0bd0000003e000000be0000403e000020be0000803e000040be0000a0"
        "3e000060be0000c03e000080be0000e03e000090be0000003f0000a0be0000103f0000"
        "b0be0000203f0000c0be0000303f0000d0be0000403f0000e0be");
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  {
    Attribute a;
    a.domain = AttributeDomain::face;
    a.name = Bytes("6d6174657269616c5f696473");
    a.semantic = Bytes("4d4154455249414c");
    a.components = 1;
    a.values = Values<std::int32_t>("11000000fcffffff43420f00");
    a.metadata.emplace(Bytes("616c706861006b6579"), Bytes("68656164"));
    a.metadata.emplace(Bytes("6d6964646c65"), Bytes("cea9"));
    a.metadata.emplace(Bytes("7a657461"), Bytes("7461696c0076"));
    mesh.attributes.push_back(std::move(a));
  }
  return mesh;
}

struct Fixture {
  Mesh legacy = MakeLegacy();
  std::vector<double> positions;
  std::vector<std::vector<MetadataEntryView>> metadata;
  std::vector<AttributeMaterializationInput> attributes;
  MeshMaterializationInput Input(bool ordered) {
    positions.clear();
    metadata.clear();
    attributes.clear();
    for (std::size_t row = 0; row < legacy.positions.size(); ++row)
      for (const auto coordinate : legacy.positions.Get(row))
        positions.push_back(coordinate);
    metadata.reserve(legacy.attributes.size());
    attributes.reserve(legacy.attributes.size());
    for (const auto& a : legacy.attributes) {
      metadata.emplace_back();
      for (const auto& [key, value] : a.metadata)
        metadata.back().push_back({key, value});
      // Actual retained order: zeta, alpha-NUL-key, middle. Legacy stays
      // sorted.
      if (ordered && !metadata.back().empty())
        std::rotate(metadata.back().begin(), metadata.back().end() - 1,
                    metadata.back().end());
      AttributeMaterializationInput input;
      input.domain = a.domain;
      input.name = a.name;
      input.semantic = a.semantic;
      input.set_index = a.set_index;
      input.components = a.components;
      input.values = std::visit(
          [](const auto& v) -> ScalarIngress { return v.Values(); }, a.values);
      if (a.offsets) input.offsets = std::span<const index_t>(*a.offsets);
      if (a.present) input.present = std::span<const std::uint8_t>(*a.present);
      input.metadata = metadata.back();
      attributes.push_back(input);
    }
    return {std::span<const double>(positions),
            PolygonTopologyInput{legacy.face_offsets, legacy.corner_vertices},
            attributes};
  }
  MaterializedMesh Capture(const ExecutionContext& context,
                           bool ordered = true) {
    const auto result =
        MaterializeMesh(Input(ordered), MeshLimits(), Work(), context);
    Require(result.status == MaterializationStatus::kSuccess &&
                result.mesh.has_value(),
            "fixture actual capture");
    return *result.mesh;
  }
};
template <class A, class B>
bool Equal(const A& a, const B& b) {
  return std::ranges::equal(a, b);
}
inline void Check(const MeshReadView& source,
                  const ChargedTriangulation& result) {
  const auto output = result.Mesh().View();
  Require(result.Inspection().IsStorageValid(), "output inspected");
  Require(Equal(result.FaceSources(),
                std::array<index_t, kOutputFaces>{0, 1, 1, 2, 2, 2}),
          "literal face map");
  Require(Equal(result.FaceOutputOffsets(),
                std::array<index_t, kFixtureFaces + 1>{0, 1, 3, 6}),
          "literal face ranges");
  Require(result.CornerSources().size() == kOutputCorners,
          "literal corner map length");
  Require(output.positions.size() == source.positions.size() &&
              std::memcmp(output.positions.data(), source.positions.data(),
                          source.positions.size_bytes()) == 0,
          "all position bits");
  Require(output.attributes.size() == kFixtureAttributes &&
              source.attributes.size() == kFixtureAttributes,
          "all nineteen channels");
  for (std::size_t corner = 0; corner < kOutputCorners; ++corner)
    Require(output.corner_vertices[corner] ==
                source.corner_vertices[result.CornerSources()[corner]],
            "authored corner not welded vertex");
  for (std::size_t face = 0; face <= kOutputFaces; ++face)
    Require(output.face_offsets[face] == face * kTriangleCorners,
            "literal triangle offsets");
  for (std::size_t ordinal = 0; ordinal < source.attributes.size(); ++ordinal) {
    const auto& a = source.attributes[ordinal];
    const auto& b = output.attributes[ordinal];
    Require(a.domain == b.domain && a.name == b.name &&
                a.semantic == b.semantic && a.set_index == b.set_index &&
                a.components == b.components &&
                a.values.index() == b.values.index(),
            "descriptor/type exact");
    Require(a.metadata.size() == b.metadata.size(), "metadata count");
    for (std::size_t i = 0; i < a.metadata.size(); ++i)
      Require(a.metadata[i].key == b.metadata[i].key &&
                  a.metadata[i].value == b.metadata[i].value,
              "ordered metadata bits");
    Require(a.offsets.has_value() == b.offsets.has_value() &&
                a.present.has_value() == b.present.has_value(),
            "optional authoredness");
    const auto rows = a.domain == AttributeDomain::vertex ? kFixtureVertices
                      : a.domain == AttributeDomain::face ? kOutputFaces
                                                          : kOutputCorners;
    std::size_t scalar = 0;
    std::visit(
        [&](const auto& original) {
          using T =
              typename std::remove_cvref_t<decltype(original)>::value_type;
          const auto actual = std::get<std::span<const T>>(b.values);
          for (std::size_t row = 0; row < rows; ++row) {
            const auto src = a.domain == AttributeDomain::vertex ? row
                             : a.domain == AttributeDomain::face
                                 ? result.FaceSources()[row]
                                 : result.CornerSources()[row];
            const auto first =
                a.offsets ? (*a.offsets)[src] : src * a.components;
            const auto last =
                a.offsets ? (*a.offsets)[src + 1] : first + a.components;
            if (a.offsets)
              Require((*b.offsets)[row] == scalar, "ragged prefix");
            if (a.present)
              Require((*b.present)[row] == (*a.present)[src], "presence exact");
            for (auto item = first; item < last; ++item) {
              Require(scalar < actual.size() &&
                          Bits(actual[scalar]) == Bits(original[item]),
                      "all typed/missing backing bits");
              ++scalar;
            }
          }
          Require(scalar == actual.size(), "no backing dropped or appended");
          if (a.offsets)
            Require(b.offsets->back() == scalar, "ragged final prefix");
        },
        a.values);
  }
}
}  // namespace meshvale::geometry::charged_example

int main(int argc, char** argv) {
  namespace g = meshvale::geometry;
  namespace f = meshvale::geometry::charged_example;
  try {
    const bool unsupported =
        argc == 2 && std::string_view(argv[1]) == "unsupported";
    f::Require(argc == 1 || unsupported, "closed expected allocation profile");
    g::ExecutionContext context({f::kWorkers, 0, f::kLedgerBytes});
    if (unsupported) {
      const auto source = f::MakeLegacy();
      const auto refused =
          g::TriangulateCharged(source, {}, f::Limits(), f::Work(), context);
      f::Require(refused.status == g::ChargedTriangulationStatus::
                                       kUnsupportedAllocationProfile &&
                     !refused.triangulation && refused.source_capture &&
                     !refused.core,
                 "actual nonempty capture-only unsupported outcome");
      const g::Mesh empty;
      const auto accepted =
          g::TriangulateCharged(empty, {}, f::Limits(), f::Work(), context);
      f::Require(accepted.status == g::ChargedTriangulationStatus::kAccepted &&
                     accepted.triangulation &&
                     accepted.triangulation->CornerSources().empty() &&
                     f::Equal(accepted.triangulation->FaceOutputOffsets(),
                              std::array<g::index_t, 1>{0}),
                 "actual zero-numerical empty result");
    } else {
      std::optional<g::ChargedTriangulation> retained;
      {
        f::Fixture fixture;
        const auto source = fixture.Capture(context);
        g::TriangulationOptions options;
        options.minimum_parallel_faces = 0;
        const auto result =
            g::TriangulateCharged(source, options, f::Limits(), f::Work());
        f::Require(result.status == g::ChargedTriangulationStatus::kAccepted &&
                       result.triangulation,
                   "actual retained public operation");
        f::Check(source.View(), *result.triangulation);
        retained = result.triangulation;
        const auto legacy_source = fixture.Capture(context, false);
        const auto legacy = g::TriangulateCharged(
            fixture.legacy, options, f::Limits(), f::Work(), context);
        f::Require(legacy.status == g::ChargedTriangulationStatus::kAccepted &&
                       legacy.triangulation,
                   "actual legacy public operation");
        f::Check(legacy_source.View(), *legacy.triangulation);
      }
      f::Require(
          retained && retained->Inspection().IsStorageValid() &&
              retained->NewRequestedBytes() == context.ActiveTrackedPayload(),
          "real complete owner survives original sources/results");
      retained.reset();
      f::Require(
          context.ActiveTrackedPayload() == 0 && context.ActiveWorkers() == 0,
          "last actual owner release");
    }
    std::cout << "Installed charged triangulation "
              << (unsupported ? "unsupported" : "qualified")
              << " profile passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
