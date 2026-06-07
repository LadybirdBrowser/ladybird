/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibURL/PublicSuffixData.h>

TEST_CASE(is_public_suffix)
{
    auto* public_suffix_data = URL::PublicSuffixData::the();

    EXPECT(public_suffix_data->is_public_suffix("com"sv));
    EXPECT(public_suffix_data->is_public_suffix("com.br"sv));
    EXPECT(public_suffix_data->is_public_suffix("co.uk"sv));
    EXPECT(public_suffix_data->is_public_suffix("ac.uk"sv));
    EXPECT(public_suffix_data->is_public_suffix("gov.uk"sv));
    EXPECT(public_suffix_data->is_public_suffix("com.au"sv));
    EXPECT(public_suffix_data->is_public_suffix("co.jp"sv));

    EXPECT(!public_suffix_data->is_public_suffix(""sv));
    EXPECT(!public_suffix_data->is_public_suffix("."sv));
    EXPECT(!public_suffix_data->is_public_suffix(".."sv));
    EXPECT(!public_suffix_data->is_public_suffix("/"sv));
    EXPECT(!public_suffix_data->is_public_suffix("not-a-public-suffix.com"sv));
    EXPECT(!public_suffix_data->is_public_suffix("com."sv));
    EXPECT(!public_suffix_data->is_public_suffix("com/"sv));
    EXPECT(!public_suffix_data->is_public_suffix("/com"sv));
    EXPECT(!public_suffix_data->is_public_suffix("not-a-public-suffix"sv));
    EXPECT(!public_suffix_data->is_public_suffix(" com"sv));
    EXPECT(!public_suffix_data->is_public_suffix("com "sv));
}

TEST_CASE(get_public_suffix)
{
    auto* public_suffix_data = URL::PublicSuffixData::the();

    EXPECT_EQ(public_suffix_data->get_public_suffix(""sv), OptionalNone {});
    EXPECT_EQ(public_suffix_data->get_public_suffix("."sv), OptionalNone {});
    EXPECT_EQ(public_suffix_data->get_public_suffix(".."sv), OptionalNone {});
    EXPECT_EQ(public_suffix_data->get_public_suffix(" "sv), OptionalNone {});
    EXPECT_EQ(public_suffix_data->get_public_suffix("/"sv), OptionalNone {});
    EXPECT_EQ(public_suffix_data->get_public_suffix("not-a-public-suffix"sv), OptionalNone {});

    EXPECT_EQ(public_suffix_data->get_public_suffix("com"sv), "com"sv);
    EXPECT_EQ(public_suffix_data->get_public_suffix("not-a-public-suffix.com"sv), "com"sv);
    EXPECT_EQ(public_suffix_data->get_public_suffix("com."sv), "com"sv);
    EXPECT_EQ(public_suffix_data->get_public_suffix(".com."sv), "com"sv);
    EXPECT_EQ(public_suffix_data->get_public_suffix("..com."sv), "com"sv);
    EXPECT_EQ(public_suffix_data->get_public_suffix("com.br"sv), "com.br"sv);
    EXPECT_EQ(public_suffix_data->get_public_suffix("not-a-public-suffix.com.br"sv), "com.br"sv);
    EXPECT_EQ(public_suffix_data->get_public_suffix("co.uk"sv), "co.uk"sv);
    EXPECT_EQ(public_suffix_data->get_public_suffix("bbc.co.uk"sv), "co.uk"sv);
    EXPECT_EQ(public_suffix_data->get_public_suffix("www.bbc.co.uk"sv), "co.uk"sv);
}
