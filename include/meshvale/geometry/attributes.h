// SPDX-License-Identifier: Apache-2.0
#ifndef MESHVALE_GEOMETRY_ATTRIBUTES_H_
#define MESHVALE_GEOMETRY_ATTRIBUTES_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace meshvale::geometry {

using index_t = std::uint64_t;
enum class AttributeDomain { vertex, face, corner };
using AttributeValues = std::variant<
    std::vector<float>, std::vector<double>, std::vector<std::int32_t>,
    std::vector<std::uint8_t>, std::vector<std::uint16_t>,
    std::vector<std::uint32_t>, std::vector<std::uint64_t>>;

struct Diagnostic {
    std::string code;
    std::string subject;
    std::optional<index_t> element;
};

struct Attribute {
    AttributeDomain domain = AttributeDomain::vertex;
    std::string name;
    std::string semantic;  // Open vocabulary; storage does not infer transfer policy.
    std::optional<std::uint32_t> set_index;
    std::uint32_t components = 1;
    AttributeValues values = std::vector<float>{};
    // Scalar offsets delimit variable-length rows; absent means dense rows.
    std::optional<std::vector<index_t>> offsets;
    // Absent means all rows authored; 0 marks a missing row, 1 an authored row.
    std::optional<std::vector<std::uint8_t>> present;
    std::map<std::string, std::string> metadata;

    [[nodiscard]] index_t value_count() const {
        return std::visit([](const auto& data) -> index_t { return data.size(); }, values);
    }
};

[[nodiscard]] inline std::vector<Diagnostic> inspect_attribute(
    const Attribute& attribute, index_t expected_rows) {
    std::vector<Diagnostic> issues;
    auto issue = [&](std::string code, std::optional<index_t> row = std::nullopt) {
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
        // Division avoids overflowing expected_rows * components on malformed input.
        if (scalar_count % attribute.components != 0 ||
            scalar_count / attribute.components != expected_rows)
            issue("attribute.row_count");
    }

    if (attribute.present) {
        if (attribute.present->size() != expected_rows) issue("attribute.presence_count");
        for (index_t i = 0; i < attribute.present->size(); ++i)
            if ((*attribute.present)[i] > 1) issue("attribute.presence_value", i);
    }
    return issues;
}

}  // namespace meshvale::geometry

#endif  // MESHVALE_GEOMETRY_ATTRIBUTES_H_
