// SPDX-License-Identifier: Apache-2.0
#include <meshvale/geometry/attribute_numerics.h>
#include <meshvale/geometry/execution.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace meshvale::geometry;
namespace {
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void Blocked(const AttributeReductionResult& result, std::string_view code,
             std::optional<index_t> row = std::nullopt) {
  Require(result.Status() == AttributeReductionStatus::kBlocked &&
              result.Presence().empty() && result.Diagnostics().size() == 1 &&
              result.Diagnostics()[0].code == code &&
              result.Diagnostics()[0].row == row,
          "wrong rejection or partial output");
  bool rejected = false;
  try {
    (void)result.CopyValues();
  } catch (const std::logic_error&) {
    rejected = true;
  }
  Require(rejected, "blocked output exported");
}
template <class T>
void AllShapes() {
  ExecutionContext execution({.worker_budget = 2});
  Attribute a;
  a.name = "authored";
  a.domain = AttributeDomain::corner;
  a.components = 3;
  a.values = std::vector<T>{3, 4, 0, 1, 2, 2, 7, 8, 9};
  a.present = std::vector<std::uint8_t>{1, 1, 0};
  a.metadata["normalized"] = "true";
  const auto original = a;
  auto sums = ComputeAttributeRowSums(a, 3, {}, execution);
  auto norms = ComputeAttributeRowSquaredNorms(a, 3, {}, execution);
  Require(sums.Status() == AttributeReductionStatus::kAccepted &&
              norms.Status() == AttributeReductionStatus::kAccepted,
          "dense reduction rejected");
  Require(std::get<ScalarBuffer<T>>(sums.CopyValues()) ==
                  std::vector<T>({7, 5, 0}) &&
              std::get<ScalarBuffer<T>>(norms.CopyValues()) ==
                  std::vector<T>({25, 9, 0}),
          "wrong dense arithmetic or encoding");
  Require(sums.Presence()[2] == 0 && std::get<T>(sums.Get(1)) == 5,
          "missing row filled or scalar cast");
  Require(a.values == original.values && a.metadata == original.metadata &&
              a.present == original.present,
          "canonical attribute changed");
  a.components = 1;
  a.values = std::vector<T>{1, 2, 3, 4, 5, 9, 8, 7, 6, 5, 4, 3, 4};
  a.offsets = std::vector<index_t>{0, 5, 5, 11, 13};
  a.present = std::vector<std::uint8_t>{1, 1, 0, 1};
  auto ragged = ComputeAttributeRowSums(a, 4, {}, execution);
  auto ragged_norm = ComputeAttributeRowSquaredNorms(a, 4, {}, execution);
  Require(std::get<ScalarBuffer<T>>(ragged.CopyValues()) ==
                  std::vector<T>({15, 0, 0, 7}) &&
              std::get<ScalarBuffer<T>>(ragged_norm.CopyValues()) ==
                  std::vector<T>({55, 0, 0, 25}),
          "ragged/unaligned-row arithmetic failed");
  Require(ragged.Presence()[1] == 1 && ragged.Presence()[2] == 0,
          "authored empty row collapsed into missing");
  auto exported = ragged.CopyValues();
  a.values = std::vector<T>{};
  auto moved = std::move(ragged);
  Require(ragged.Status() == AttributeReductionStatus::kBlocked &&
              ragged.Presence().empty() && std::get<T>(moved.Get(3)) == 7,
          "move or source mutation invalidated owner");
  auto* alias = &moved;
  moved = std::move(*alias);
  Require(std::get<T>(moved.Get(0)) == 15, "self move lost owner");
  moved = {};
  Require(std::get<ScalarBuffer<T>>(exported)[3] == 7,
          "export retained a stale view");
}
void FloatingBitsAndDomains() {
  ExecutionContext execution({.worker_budget = 1});
  Attribute a;
  a.name = "weights";
  a.components = 2;
  const auto nan = std::bit_cast<double>(std::uint64_t{0x7ff8000000001234});
  a.values = std::vector<double>{nan, -0.0, 3, 4};
  a.present = std::vector<std::uint8_t>{0, 1};
  const auto bytes = std::get<ScalarBuffer<double>>(a.values);
  auto r = ComputeAttributeRowSquaredNorms(a, 2, {}, execution);
  Require(r.Status() == AttributeReductionStatus::kAccepted &&
              std::get<double>(r.Get(1)) == 25,
          "missing NaN evaluated");
  Require(
      std::memcmp(bytes.data(), std::get<ScalarBuffer<double>>(a.values).data(),
                  bytes.size() * sizeof(double)) == 0,
      "double NaN/signed-zero source bits changed");
  (*a.present)[0] = 1;
  Blocked(ComputeAttributeRowSums(a, 2, {}, execution),
          "numerical.nonfinite_value", 0);
  a.values = std::vector<double>{std::numeric_limits<double>::max(),
                                 std::numeric_limits<double>::max(), 0, 0};
  Blocked(ComputeAttributeRowSums(a, 2, {}, execution),
          "numerical.nonfinite_result", 0);
  Blocked(ComputeAttributeRowSquaredNorms(a, 2, {}, execution),
          "numerical.nonfinite_result", 0);
  a.values = std::vector<float>{std::bit_cast<float>(std::uint32_t{0x7fc01234}),
                                -0.0F, 3, 4};
  a.present = std::vector<std::uint8_t>{0, 1};
  const auto float_bytes = std::get<ScalarBuffer<float>>(a.values);
  auto f = ComputeAttributeRowSums(a, 2, {}, execution);
  Require(std::get<float>(f.Get(1)) == 7 &&
              std::memcmp(float_bytes.data(),
                          std::get<ScalarBuffer<float>>(a.values).data(),
                          float_bytes.size() * sizeof(float)) == 0,
          "float source bits or encoding changed");
  // Independent corner UV sets are reduced as separate channels with no weld.
  a.domain = AttributeDomain::corner;
  a.semantic = "texcoord";
  a.set_index = 0;
  a.present.reset();
  a.values = std::vector<float>{3, 4, 0, 2};
  auto uv0 = ComputeAttributeRowSquaredNorms(a, 2, {}, execution);
  a.name = "lightmap";
  a.set_index = 1;
  a.values = std::vector<float>{1, 0, 6, 8};
  auto uv1 = ComputeAttributeRowSquaredNorms(a, 2, {}, execution);
  Require(std::get<float>(uv0.Get(0)) == 25 &&
              std::get<float>(uv1.Get(0)) == 1 &&
              std::get<float>(uv1.Get(1)) == 100,
          "UV sets/seam rows collapsed");
  for (const auto* semantic : {"normal", "tangent", "color"}) {
    a.semantic = semantic;
    a.components = 3;
    a.values = std::vector<float>{3, 4, 0};
    Require(
        std::get<float>(
            ComputeAttributeRowSquaredNorms(a, 1, {}, execution).Get(0)) == 25,
        "semantic norm failed");
  }
}
void IntegerChecks() {
  ExecutionContext e({.worker_budget = 1});
  Attribute a;
  a.name = "joints";
  a.values = std::vector<std::uint64_t>{
      9007199254740993ULL, std::numeric_limits<std::uint64_t>::max()};
  auto ids = ComputeAttributeRowSums(a, 2, {}, e);
  Require(std::get<std::uint64_t>(ids.Get(0)) == 9007199254740993ULL &&
              std::get<std::uint64_t>(ids.Get(1)) ==
                  std::numeric_limits<std::uint64_t>::max(),
          "joint identifier cast to double");
  Blocked(ComputeAttributeRowSquaredNorms(a, 2, {}, e),
          "numerical.integer_overflow", 0);
  a.components = 2;
  a.values = std::vector<std::uint8_t>{255, 1};
  Blocked(ComputeAttributeRowSums(a, 1, {}, e), "numerical.integer_overflow",
          0);
  a.components = 1;
  a.values = std::vector<std::uint8_t>{16};
  Blocked(ComputeAttributeRowSquaredNorms(a, 1, {}, e),
          "numerical.integer_overflow", 0);
  a.components = 2;
  a.values =
      std::vector<std::int32_t>{std::numeric_limits<std::int32_t>::max(), 1};
  Blocked(ComputeAttributeRowSums(a, 1, {}, e), "numerical.integer_overflow",
          0);
  a.values =
      std::vector<std::int32_t>{std::numeric_limits<std::int32_t>::min(), -1};
  Blocked(ComputeAttributeRowSums(a, 1, {}, e), "numerical.integer_overflow",
          0);
  a.components = 1;
  a.values =
      std::vector<std::int32_t>{std::numeric_limits<std::int32_t>::min()};
  Blocked(ComputeAttributeRowSquaredNorms(a, 1, {}, e),
          "numerical.integer_overflow", 0);
  a.values = std::vector<std::int32_t>{-46340};
  Require(
      std::get<std::int32_t>(
          ComputeAttributeRowSquaredNorms(a, 1, {}, e).Get(0)) == 2147395600,
      "negative square wrongly overflowed");
  a.components = 4;
  a.values = std::vector<std::int32_t>{std::numeric_limits<std::int32_t>::max(),
                                       1, -1, 0};
  Blocked(ComputeAttributeRowSums(a, 1, {}, e), "numerical.integer_overflow",
          0);
}
void ShapeAndLifetime() {
  ExecutionContext e({.worker_budget = 1});
  Attribute a;
  a.name = "raw";
  a.values = std::vector<double>{1, 2, 3, 4};
  a.components = 2;
  Blocked(
      ComputeAttributeRowSums(a, std::numeric_limits<index_t>::max(), {}, e),
      "numerical.host_extent");
  Blocked(ComputeAttributeRowSums(a, 3, {}, e), "attribute.row_count");
  a.offsets = std::vector<index_t>{0, 3, 4};
  Blocked(ComputeAttributeRowSums(a, 2, {}, e), "attribute.component_alignment",
          1);
  a.offsets = std::vector<index_t>{0, 6, 4};
  Blocked(ComputeAttributeRowSums(a, 2, {}, e), "attribute.offset_range", 1);
  a.offsets = std::vector<index_t>{0, 4, 2, 4};
  Blocked(ComputeAttributeRowSums(a, 3, {}, e), "attribute.offset_order", 2);
  a.offsets.reset();
  a.present = std::vector<std::uint8_t>{1, 2};
  Blocked(ComputeAttributeRowSums(a, 2, {}, e), "attribute.presence_value", 1);
  a.present = std::vector<std::uint8_t>{1};
  Blocked(ComputeAttributeRowSums(a, 2, {}, e), "attribute.presence_count");
  a.present.reset();
  a.components = 0;
  Blocked(ComputeAttributeRowSums(a, 2, {}, e), "attribute.zero_components");
  a.components = 2;
  a.domain = static_cast<AttributeDomain>(99);
  Blocked(ComputeAttributeRowSums(a, 2, {}, e), "attribute.invalid_domain");
  AttributeReductionResult owned;
  {
    ExecutionContext local({.worker_budget = 1});
    Attribute source;
    source.name = "temporary";
    source.values = std::vector<float>{3, 4};
    source.components = 2;
    owned = ComputeAttributeRowSums(source, 1, {}, local);
  }
  Require(std::get<float>(owned.Get(0)) == 7,
          "source/context destruction invalidated result");
  bool out = false;
  try {
    (void)owned.Get(1);
  } catch (const std::out_of_range&) {
    out = true;
  }
  Require(out, "unchecked output index");
  a.domain = AttributeDomain::vertex;
  a.components = 1;
  a.values = std::vector<double>{};
  auto empty = ComputeAttributeRowSums(a, 0, {}, e);
  Require(empty.Status() == AttributeReductionStatus::kAccepted &&
              empty.Presence().empty() &&
              std::get<ScalarBuffer<double>>(empty.CopyValues()).empty(),
          "empty encoding lost");
}
void BudgetAndWorkers() {
  Attribute a;
  a.name = "weights";
  a.components = 2;
  a.values = std::vector<double>(4096, 0.25);
  std::size_t exact;
  {
    ExecutionContext e({.worker_budget = 1});
    auto r = ComputeAttributeRowSums(a, 2048, {}, e);
    exact = r.PeakTrackedPayloadBytes();
    Require(e.PeakTrackedPayload() == exact &&
                e.ActiveTrackedPayload() > 2048 * (sizeof(double) + 1),
            "full block/header accounting missing");
  }
  {
    ExecutionContext e(
        {.worker_budget = 1, .tracked_payload_budget_bytes = exact});
    auto r = ComputeAttributeRowSums(a, 2048, {}, e);
    Require(r.Status() == AttributeReductionStatus::kAccepted,
            "exact admitted budget rejected");
    r = {};
    Require(e.ActiveTrackedPayload() == 0,
            "result did not release storage charge");
  }
  {
    ExecutionContext e(
        {.worker_budget = 1, .tracked_payload_budget_bytes = exact - 1});
    Blocked(ComputeAttributeRowSums(a, 2048, {}, e),
            "numerical.payload_budget");
    Require(e.ActiveTrackedPayload() == 0, "failed admission leaked");
  }
  ExecutionContext parallel({.worker_budget = 2});
  auto p = ComputeAttributeRowSums(a, 2048, {}, parallel);
  Require(p.WorkersUsed() == 2 && parallel.ActiveWorkers() == 0,
          "real admitted workers not joined");
  ExecutionContext serial({.worker_budget = 1});
  auto s = ComputeAttributeRowSums(a, 2048, {}, serial);
  Require(p.CopyValues() == s.CopyValues(), "serial/parallel result differed");
  auto occupied = TryReserveWorkers(parallel, 2);
  auto fallback = ComputeAttributeRowSums(a, 2048, {}, parallel);
  Require(fallback.WorkersUsed() == 0 &&
              fallback.SerialReason() == "shared worker budget unavailable" &&
              fallback.CopyValues() == s.CopyValues(),
          "occupied worker fallback failed");
  occupied.Reset();
  auto tiny = ComputeAttributeRowSums(a, 2048, {.minimum_parallel_rows = 4096},
                                      parallel);
  Require(tiny.WorkersUsed() == 0 &&
              tiny.SerialReason() == "below parallel row threshold",
          "tiny fallback dishonest");
  std::stop_source stop;
  stop.request_stop();
  auto canceled =
      ComputeAttributeRowSums(a, 2048, {}, parallel, stop.get_token());
  Require(canceled.Status() == AttributeReductionStatus::kCanceled &&
              canceled.Presence().empty(),
          "pre-cancel exposed output");
  Attribute large;
  large.name = "large";
  large.components = 32;
  large.values = std::vector<double>(1024 * 1024, 0.25);
  ExecutionContext during({.worker_budget = 2});
  std::stop_source later;
  std::atomic<bool> finished = false;
  std::atomic<bool> observed = false;
  std::jthread canceler([&] {
    while (!finished.load()) {
      if (during.ActiveWorkers() == 2) {
        observed = true;
        later.request_stop();
        return;
      }
      std::this_thread::yield();
    }
  });
  auto interrupted = ComputeAttributeRowSquaredNorms(large, 32768, {}, during,
                                                     later.get_token());
  finished = true;
  canceler.join();
  Require(observed &&
              interrupted.Status() == AttributeReductionStatus::kCanceled &&
              interrupted.Presence().empty() && during.ActiveWorkers() == 0 &&
              during.ActiveTrackedPayload() == 0,
          "in-worker cancellation leaked or published");
}
}  // namespace
int main() {
  try {
    AllShapes<float>();
    AllShapes<double>();
    AllShapes<std::int32_t>();
    AllShapes<std::uint8_t>();
    AllShapes<std::uint16_t>();
    AllShapes<std::uint32_t>();
    AllShapes<std::uint64_t>();
    FloatingBitsAndDomains();
    IntegerChecks();
    ShapeAndLifetime();
    BudgetAndWorkers();
    std::cout << "Checked attribute reductions: all seven scalar types, "
                 "dense/ragged/missing/corner rows, bits, owned lifetime, full "
                 "budgets and joined workers passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
