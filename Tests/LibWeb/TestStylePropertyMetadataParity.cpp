/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/RustStyleBridge.h>

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
    rust_property_metadata_bounds(&first_longhand, &last_longhand, &first_inherited, &last_inherited);
    EXPECT_EQ(first_longhand, to_underlying(first_longhand_property_id));
    EXPECT_EQ(last_longhand, to_underlying(last_longhand_property_id));
    EXPECT_EQ(first_inherited, to_underlying(first_inherited_property_id));
    EXPECT_EQ(last_inherited, to_underlying(last_inherited_property_id));
}

TEST_CASE(inherited_flags_match)
{
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i)
        EXPECT_EQ(rust_property_metadata_is_inherited(i), is_inherited_property(static_cast<PropertyID>(i)));
}

TEST_CASE(computation_order_matches)
{
    size_t length = 0;
    auto const* order = rust_property_metadata_computation_order(&length);
    auto const& cpp_order = property_computation_order();
    EXPECT_EQ(length, cpp_order.size());
    for (size_t i = 0; i < min(length, cpp_order.size()); ++i)
        EXPECT_EQ(order[i], to_underlying(cpp_order[i]));
}

TEST_CASE(requires_computation_levels_match)
{
    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        auto level = rust_property_metadata_requires_computation_level(i);
        EXPECT_EQ(level >= 1, property_requires_computation_with_cascaded_value(property_id));
        EXPECT_EQ(level >= 2, property_requires_computation_with_initial_value(property_id));
        EXPECT_EQ(level >= 3, property_requires_computation_with_inherited_value(property_id));
    }
}

}
