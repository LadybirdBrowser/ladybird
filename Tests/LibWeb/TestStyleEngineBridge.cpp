/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16FlyString.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/StyleEngineBridge.h>

TEST_CASE(interned_atoms_are_released_with_the_style_engine)
{
    auto initial_fly_string_count = Utf16FlyString::number_of_utf16_fly_strings();
    {
        auto name = Utf16FlyString::from_utf8_without_validation("style-engine-atom-lifetime-test-name"sv);
        EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), initial_fly_string_count + 1);

        Web::CSS::StyleEngine engine(Web::CSS::StyleEngine::DeviceClass::ForegroundDesktop);
        engine.intern_atom(name);
    }
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), initial_fly_string_count);
}
