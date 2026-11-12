/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Environment.h>
#include <LibTest/TestCase.h>
#include <LibTest/TestSuite.h>

static int setup_value;

TEST_SETUP
{
    setup_value = 1;
}

TEST_CASE(setup_and_process_state_are_isolated)
{
    EXPECT_EQ(setup_value, 1);
    EXPECT(!Core::Environment::has("LADYBIRD_TEST_SUITE_STATE"sv));
    MUST(Core::Environment::set("LADYBIRD_TEST_SUITE_STATE"sv, "changed"sv, Core::Environment::Overwrite::Yes));
}

TEST_CASE(suites_are_selected_by_name)
{
    Test::TestSuite suite;
    auto test_case = make_ref_counted<Test::TestCase>("example"sv, "/Tests/TestExample.cpp", [] { });
    suite.add_case(test_case);
    suite.add_case(make_ref_counted<Test::TestCase>("example"sv, "/Tests/TestExamplePeer.cpp", [] { }));

    auto matches = suite.find_cases("*", "TestExample"sv);
    EXPECT_EQ(matches.size(), 1u);
    if (matches.size() == 1)
        EXPECT_EQ(matches.first().ptr(), test_case.ptr());

    EXPECT(suite.find_cases("*", "TestExample.cpp"sv).is_empty());
    EXPECT(suite.find_cases("*", "/Tests/TestExample.cpp"sv).is_empty());
    EXPECT(suite.find_cases("*", "TestExam"sv).is_empty());
    EXPECT_EQ(suite.find_cases("TestExample::*").size(), 1u);
    EXPECT(suite.find_cases("TestExample.cpp::*").is_empty());
}
