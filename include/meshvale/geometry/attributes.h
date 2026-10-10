// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_ATTRIBUTES_H_
#define MESHVALE_GEOMETRY_ATTRIBUTES_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace meshvale::geometry {

using index_t = std::uint64_t;
enum class AttributeDomain { vertex, face, corner };
using AttributeValues =
    std::variant<std::vector<float>, std::vector<double>,
                 std::vector<std::int32_t>, std::vector<std::uint8_t>,
                 std::vector<std::uint16_t>, std::vector<std::uint32_t>,
                 std::vector<std::uint64_t>>;

struct Diagnostic {
  std::string code;
  std::string subject;
  std::optional<index_t> element;
};

struct Attribute {
  AttributeDomain domain = AttributeDomain::vertex;
  std::string name;
  std::string
      semantic;  // Open vocabulary; storage does not infer transfer policy.
  std::optional<std::uint32_t> set_index;
  std::uint32_t components = 1;
  AttributeValues values = std::vector<float>{};
  // Scalar offsets delimit variable-length rows; absent means dense rows.
  std::optional<std::vector<index_t>> offsets;
  // Absent means all rows authored; 0 marks a missing row, 1 an authored row.
  std::optional<std::vector<std::uint8_t>> present;
  std::map<std::string, std::string> metadata;

  [[nodiscard]] index_t value_count() const;
};

[[nodiscard]] std::vector<Diagnostic> inspect_attribute(
    const Attribute& attribute, index_t expected_rows);

}  // namespace meshvale::geometry

#endif  // MESHVALE_GEOMETRY_ATTRIBUTES_H_
