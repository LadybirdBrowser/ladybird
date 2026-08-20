/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Dump.h>

TEST_CASE(selector_dumps_are_indented_and_newline_terminated)
{
    StringBuilder builder;
    builder.append("Before\n"sv);
    Web::dump_serialized_selector(builder, ".first"sv, 1);
    Web::dump_serialized_selector(builder, ".second"sv, 1);
    builder.append("After\n"sv);

    EXPECT_EQ(builder.string_view(), R"~~~(Before
  CSS::Selector: .first
  CSS::Selector: .second
After
)~~~"sv);
}
