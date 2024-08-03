/*
 * Copyright (c) 2024, Alisson Lauffer <alissonvitortc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibWeb/DOMURL/URLSearchParams.h>

TEST_CASE(url_search_params)
{
    auto params = MUST(Web::DOMURL::url_decode("test test+test%20test=test test+test%20test"sv));

    EXPECT_EQ(params[0].name, "test test test test"sv);
    EXPECT_EQ(params[0].value, "test test test test"sv);
}
