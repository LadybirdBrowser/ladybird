/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/RustStyleBridge.h>
#include <LibWeb/CSS/StyleComputer.h>

// The Rust style computation core generates its property metadata tables from
// Properties.json independently of the C++ generator. These tests compare
// every table so the two generators cannot drift apart.

namespace Web::CSS {

TEST_CASE(property_bounds_match)
{
    u16 first_longhand = 0;
    u16 last_longhand = 0;
    u16 first_inherited = 0;
    u16 last_inherited = 0;
    invoke_rust_property_metadata_bounds(&first_longhand, &last_longhand, &first_inherited, &last_inherited);
    EXPECT_EQ(first_longhand, to_underlying(first_longhand_property_id));
    EXPECT_EQ(last_longhand, to_underlying(last_longhand_property_id));
    EXPECT_EQ(first_inherited, to_underlying(first_inherited_property_id));
    EXPECT_EQ(last_inherited, to_underlying(last_inherited_property_id));
}

TEST_CASE(inherited_flags_match)
{
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i)
        EXPECT_EQ(invoke_rust_property_metadata_is_inherited(i), is_inherited_property(static_cast<PropertyID>(i)));
}

TEST_CASE(computation_order_matches)
{
    size_t length = 0;
    auto const* order = invoke_rust_property_metadata_computation_order(&length);
    auto const& cpp_order = property_computation_order();
    EXPECT_EQ(length, cpp_order.size());
    for (size_t i = 0; i < min(length, cpp_order.size()); ++i)
        EXPECT_EQ(order[i], to_underlying(cpp_order[i]));
}

TEST_CASE(logical_alias_mapping_matches)
{
    StyleComputer::ensure_style_metadata_tables_installed();
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        for (u8 writing_mode = 0; writing_mode < 5; ++writing_mode) {
            for (u8 direction = 0; direction < 2; ++direction) {
                auto expected = property_is_logical_alias(property_id)
                    ? map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { static_cast<WritingMode>(writing_mode), static_cast<Direction>(direction) })
                    : property_id;
                EXPECT_EQ(invoke_rust_map_logical_alias_to_physical(i, writing_mode, direction), to_underlying(expected));
            }
        }
    }
}

TEST_CASE(physical_to_logical_mapping_matches)
{
    StyleComputer::ensure_style_metadata_tables_installed();
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        for (u8 writing_mode = 0; writing_mode < 5; ++writing_mode) {
            for (u8 direction = 0; direction < 2; ++direction) {
                auto expected = property_is_logical_alias(property_id)
                    ? property_id
                    : map_physical_property_to_logical_alias(property_id, LogicalAliasMappingContext { static_cast<WritingMode>(writing_mode), static_cast<Direction>(direction) });
                EXPECT_EQ(invoke_rust_map_physical_to_logical_alias(i, writing_mode, direction), to_underlying(expected));
            }
        }
    }
}

TEST_CASE(shorthand_expansions_match)
{
    for (auto i = to_underlying(first_property_id); i <= to_underlying(last_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        EXPECT_EQ(invoke_rust_property_metadata_is_shorthand(i), property_is_shorthand(property_id));
        if (!property_is_shorthand(property_id))
            continue;
        size_t length = 0;
        auto const* longhands = invoke_rust_property_metadata_longhands_for_shorthand(i, &length);
        auto const& cpp_longhands = longhands_for_shorthand(property_id);
        EXPECT_EQ(length, cpp_longhands.size());
        for (size_t j = 0; j < min(length, cpp_longhands.size()); ++j)
            EXPECT_EQ(longhands[j], to_underlying(cpp_longhands[j]));
    }
}

TEST_CASE(initial_value_table_matches)
{
    StyleComputer::ensure_style_metadata_tables_installed();
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto entry = invoke_rust_style_metadata_initial_value(i);
        auto initial_value = property_initial_value(static_cast<PropertyID>(i));
        EXPECT_EQ(entry.shell, static_cast<void const*>(initial_value.ptr()));
        EXPECT_EQ(entry.data, static_cast<void const*>(initial_value->rust_style_value_data()));
    }
}

TEST_CASE(requires_computation_levels_match)
{
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        auto level = invoke_rust_property_metadata_requires_computation_level(i);
        EXPECT_EQ(level >= 1, property_requires_computation_with_cascaded_value(property_id));
        EXPECT_EQ(level >= 2, property_requires_computation_with_initial_value(property_id));
        EXPECT_EQ(level >= 3, property_requires_computation_with_inherited_value(property_id));
    }
}

}
