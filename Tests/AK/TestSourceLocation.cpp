/*
 * Copyright (c) 2021, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2021, Brian Gianforcaro <bgianf@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/SourceLocation.h>
#include <AK/StringView.h>

TEST_CASE(basic_scenario)
{
    auto location = SourceLocation::current();
    EXPECT_EQ(__LINE__ - 1u, location.line_number());

    // Obviously not the prettiest way.
    StringView function { __FUNCTION__, strlen(__FUNCTION__) };
    StringView file { __FILE__, strlen(__FILE__) };

    EXPECT_EQ(function, location.function_name());
    EXPECT_EQ(file, location.filename());
}

static StringView test_default_arg(SourceLocation const& loc = SourceLocation::current())
{
    return loc.function_name();
}

TEST_CASE(default_arg_scenario)
{
    auto actual_calling_function = test_default_arg();
    auto expected_calling_function = StringView { __FUNCTION__, strlen(__FUNCTION__) };

    EXPECT_EQ(expected_calling_function, actual_calling_function);
}
