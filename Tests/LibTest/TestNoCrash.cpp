/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

TEST_CASE(raise)
{
    // This should never crash
    EXPECT_NO_DEATH([] { }());
}
