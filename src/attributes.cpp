// SPDX-License-Identifier: Apache-2.0
#include "meshvale/geometry/attributes.h"

#include <utility>
#include <variant>

namespace meshvale::geometry {

index_t Attribute::value_count() const {
  return std::visit([](const auto& data) -> index_t { return data.size(); },
                    values);
}

std::vector<Diagnostic> inspect_attribute(const Attribute& attribute,
                                          index_t expected_rows) {
  std::vector<Diagnostic> issues;
  auto issue = [&](std::string code,
                   std::optional<index_t> row = std::nullopt) {
    issues.push_back({std::move(code), attribute.name, row});
  };
  if (attribute.name.empty()) issue("attribute.empty_name");
  if (attribute.components == 0) issue("attribute.zero_components");

  const auto scalar_count = attribute.value_count();
  if (attribute.offsets) {
    const auto& offsets = *attribute.offsets;
    if (offsets.empty()) {
      issue("attribute.empty_offsets");
    } else {
      if (offsets.size() - 1 != expected_rows) issue("attribute.row_count");
      if (offsets.front() != 0) issue("attribute.offset_start");
      if (offsets.back() != scalar_count) issue("attribute.offset_end");
      for (index_t i = 0; i < offsets.size(); ++i) {
        if (offsets[i] > scalar_count) issue("attribute.offset_range", i);
        if (i > 0 && offsets[i] < offsets[i - 1])
          issue("attribute.offset_order", i);
        if (attribute.components != 0 && offsets[i] % attribute.components != 0)
          issue("attribute.component_alignment", i);
      }
    }
  } else if (attribute.components != 0) {
    // Division avoids overflowing expected_rows * components on malformed
    // input.
    if (scalar_count % attribute.components != 0 ||
        scalar_count / attribute.components != expected_rows)
      issue("attribute.row_count");
  }

  if (attribute.present) {
    if (attribute.present->size() != expected_rows)
      issue("attribute.presence_count");
    for (index_t i = 0; i < attribute.present->size(); ++i)
      if ((*attribute.present)[i] > 1) issue("attribute.presence_value", i);
  }
  return issues;
}

}  // namespace meshvale::geometry
