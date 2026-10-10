// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/attributes.h>
#include <meshvale/geometry/scalar_buffer.h>

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace meshvale::geometry;
void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
template <class Exception, class Operation>
void Reject(Operation operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const Exception&) {
    rejected = true;
  }
  Require(rejected, "storage failure type");
}
template <class T>
std::vector<T> Fixture() {
  if constexpr (std::same_as<T, float>) {
    return {std::bit_cast<float>(0x80000000U),
            std::bit_cast<float>(0x7fc12345U),
            std::bit_cast<float>(0xff812345U), 1.25F};
  } else if constexpr (std::same_as<T, double>) {
    return {std::bit_cast<double>(0x8000000000000000ULL),
            std::bit_cast<double>(0x7ff8123456789abcULL),
            std::bit_cast<double>(0xfff0123456789abcULL), 1.25};
  } else {
    return {std::numeric_limits<T>::min(), std::numeric_limits<T>::max(), T{0},
            T{1}};
  }
}
template <class T>
std::vector<std::byte> Bytes(const ScalarBuffer<T>& values) {
  std::vector<std::byte> bytes(values.size() * sizeof(T));
  values.CopyBytesTo(bytes);
  return bytes;
}
template <class T>
void Storage() {
  ScalarBuffer<T> empty;
  empty.resize(0);
  empty.reserve(0);
  Require(empty.empty() && empty.capacity() == 0 && empty.data() == nullptr &&
              empty.begin() == empty.end(),
          "empty storage allocated");
  empty.AssignBytes({});
  empty.CopyBytesTo({});
  Reject<std::out_of_range>([&] { (void)empty.front(); });
  Reject<std::out_of_range>([&] { (void)empty.back(); });

  const auto input = Fixture<T>();
  ScalarBuffer<T> values(input);
  const auto expected = Bytes(values);
  Require(std::memcmp(expected.data(), input.data(), expected.size()) == 0,
          "ingress changed scalar bits");
  values.reserve(17);
  Require(values.size() == input.size() && values.capacity() == 17 &&
              Bytes(values) == expected &&
              values.Values().data() == values.data(),
          "reserve/layout changed logical bytes");
  auto copied = values;
  Require(copied.capacity() == copied.size() &&
              copied.data() != values.data() && Bytes(copied) == expected,
          "copy shares storage or exports capacity");
  copied[0] = T{2};
  Require(Bytes(values) == expected, "copy mutation changed source");
  auto* alias = &values;
  values = *alias;
  values = std::move(*alias);
  Require(Bytes(values) == expected, "self assignment changed bits");
  auto moved = std::move(values);
  Require(values.empty() && values.capacity() == 0 && Bytes(moved) == expected,
          "move ownership");
  values.push_back(T{3});
  Require(values[0] == T{3}, "moved source not reusable");
  moved.resize(8);
  for (std::size_t i = input.size(); i < moved.size(); ++i)
    Require(moved[i] == T{}, "new scalar slots uninitialized");
  Require(std::memcmp(moved.data(), expected.data(), expected.size()) == 0,
          "resize changed prefix bits");
  moved.resize(input.size());
  moved.push_back(moved[1]);
  Require(std::memcmp(&moved.back(), &input[1], sizeof(T)) == 0,
          "aliased append changed bits");
  Reject<std::length_error>(
      [&] { moved.reserve(std::numeric_limits<std::size_t>::max()); });
  Reject<std::length_error>([&] {
    moved.reserve(std::numeric_limits<std::size_t>::max() / sizeof(T));
  });
  Reject<std::out_of_range>([&] { (void)moved[moved.size()]; });
  const auto old = Bytes(moved);
  std::vector<std::byte> misaligned(old.size() + 1);
  std::memcpy(misaligned.data() + 1, old.data(), old.size());
  moved.AssignBytes(std::span(misaligned).subspan(1));
  Require(Bytes(moved) == old, "misaligned byte input changed bits");
  moved.CopyBytesTo(std::as_writable_bytes(moved.Values()));
  Require(Bytes(moved) == old, "overlapping byte output changed bits");
  moved.AssignBytes(std::as_bytes(moved.Values()).subspan(sizeof(T)));
  Require(Bytes(moved) ==
              std::vector<std::byte>(old.begin() + sizeof(T), old.end()),
          "overlapping byte replacement changed bits");
  Reject<std::invalid_argument>([&] { moved.CopyBytesTo(misaligned); });
  if constexpr (sizeof(T) > 1)
    Reject<std::invalid_argument>(
        [&] { moved.AssignBytes(std::span(misaligned).first(1)); });
  const auto capacity = moved.capacity();
  moved.clear();
  Require(moved.empty() && moved.capacity() == capacity,
          "clear changed capacity");
  moved.resize(2);
  Require(moved[0] == T{} && moved[1] == T{}, "resize after clear not zeroed");

  ScalarBuffer<T> inserted{T{0}, T{1}, T{2}, T{3}};
  const auto position =
      inserted.insert(inserted.begin() + 2, inserted.begin(), inserted.end());
  Require(position == inserted.begin() + 2 &&
              inserted == ScalarBuffer<T>{T{0}, T{1}, T{0}, T{1}, T{2}, T{3},
                                          T{2}, T{3}},
          "self-overlapping insertion");
  ScalarBuffer<T> range(inserted.begin() + 2, inserted.begin() + 6);
  Require(range == ScalarBuffer<T>{T{0}, T{1}, T{2}, T{3}},
          "pointer-range copy");
  ScalarBuffer<T> zeros(3);
  Require(zeros == ScalarBuffer<T>{T{}, T{}, T{}}, "count ingress not zeroed");
}
void Channels() {
  Attribute weights;
  weights.name = "weights";
  weights.values = std::vector<double>{0.1, 0.2, 0.3, 0.4, 0.5, -0.0};
  weights.offsets = std::vector<index_t>{0, 5, 5, 6};
  weights.present = std::vector<std::uint8_t>{1, 1, 0};
  weights.metadata["normalized"] = "false";
  Require(inspect_attribute(weights, 3).empty(), "ragged rows changed");
  auto copy = weights;
  std::get<ScalarBuffer<double>>(copy.values)[0] = 4;
  Require(std::get<ScalarBuffer<double>>(weights.values)[0] == 0.1 &&
              weights.offsets == copy.offsets &&
              weights.present == copy.present,
          "channel copy ownership");
  weights.offsets = std::vector<index_t>{1, 99, 3};
  Require(!inspect_attribute(weights, 3).empty() &&
              *weights.offsets == std::vector<index_t>{1, 99, 3},
          "malformed input rewritten");
  Attribute joints;
  joints.name = "joint_indices";
  joints.values =
      std::vector<std::uint64_t>{9007199254740993ULL, 18446744073709551615ULL};
  Require(joints.values.index() == 6 &&
              std::get<ScalarBuffer<std::uint64_t>>(joints.values)[0] ==
                  9007199254740993ULL,
          "integer dispatch or bits changed");
  ScalarBuffer<double> positive{0.0}, negative{-0.0};
  Require(positive == negative && Bytes(positive) != Bytes(negative),
          "scalar equality lost prior IEEE semantics");
  const ScalarBuffer<double> nan{std::bit_cast<double>(0x7ff8123456789abcULL)};
  Require(!(nan == nan), "NaN equality changed");
}
}  // namespace
int main() {
  try {
    Storage<float>();
    Storage<double>();
    Storage<std::int32_t>();
    Storage<std::uint8_t>();
    Storage<std::uint16_t>();
    Storage<std::uint32_t>();
    Storage<std::uint64_t>();
    Channels();
    std::cout
        << "Seven Eigen scalar owners: exact bits, zero initialization, "
           "copy/move/overlap, ceilings and ragged/missing channels passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
