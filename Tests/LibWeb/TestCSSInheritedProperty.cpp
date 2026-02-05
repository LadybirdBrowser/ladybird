/*
 * Copyright (c) 2024, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web {

TEST_CASE(is_inherited_property_test)
{
    // Test inherited properties
    EXPECT(CSS::is_inherited_property(CSS::PropertyID::Color));
    EXPECT(CSS::is_inherited_property(CSS::PropertyID::FontSize));
    EXPECT(CSS::is_inherited_property(CSS::PropertyID::Visibility));

    // Test non-inherited properties
    EXPECT(!CSS::is_inherited_property(CSS::PropertyID::Margin));
    EXPECT(!CSS::is_inherited_property(CSS::PropertyID::Padding));
    EXPECT(!CSS::is_inherited_property(CSS::PropertyID::Border));

    // Edge cases
    EXPECT(!CSS::is_inherited_property(CSS::PropertyID::All));
}

}
