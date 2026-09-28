/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWebCommon/CSS/PropertyList.h>

TEST_CASE(property_list_matches_property_ids)
{
    auto property_list = Web::CSS::property_list();
    auto first = to_underlying(Web::CSS::first_property_id);
    auto last = to_underlying(Web::CSS::last_property_id);
    EXPECT_EQ(property_list.size(), static_cast<size_t>(last - first + 1));

    for (size_t i = 0; i < property_list.size(); ++i) {
        auto property_id = static_cast<Web::CSS::PropertyID>(first + i);
        EXPECT_EQ(Web::CSS::string_from_property_id(property_id).to_utf16_string().to_utf8(), property_list[i].name);
        EXPECT_EQ(Web::CSS::is_inherited_property(property_id), property_list[i].is_inherited);
    }
}
