/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Badge.h>

TEST_CASE(should_provide_underlying_type)
{
    static_assert(IsSame<int, Badge<int>::Type>);
}
