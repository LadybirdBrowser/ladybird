/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Environment.h>
#include <LibTest/TestCase.h>

static int setup_value;

TEST_SETUP
{
    setup_value = 2;
}

TEST_CASE(setup_and_process_state_are_isolated)
{
    EXPECT_EQ(setup_value, 2);
    EXPECT(!Core::Environment::has("LADYBIRD_TEST_SUITE_STATE"sv));
    MUST(Core::Environment::set("LADYBIRD_TEST_SUITE_STATE"sv, "changed"sv, Core::Environment::Overwrite::Yes));
}
