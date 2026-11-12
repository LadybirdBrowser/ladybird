/*
 * Copyright (c) 2023, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/PropertyID.h>

TEST_CASE(basic)
{
    EXPECT_EQ(Web::CSS::keyword_from_string("italic"sv).value(), Web::CSS::Keyword::Italic);
    EXPECT_EQ(Web::CSS::keyword_from_string("inline"sv).value(), Web::CSS::Keyword::Inline);
    EXPECT_EQ(Web::CSS::keyword_from_string("small"sv).value(), Web::CSS::Keyword::Small);
    EXPECT_EQ(Web::CSS::keyword_from_string("smalL"sv).value(), Web::CSS::Keyword::Small);
    EXPECT_EQ(Web::CSS::keyword_from_string("SMALL"sv).value(), Web::CSS::Keyword::Small);
    EXPECT_EQ(Web::CSS::keyword_from_string("Small"sv).value(), Web::CSS::Keyword::Small);
    EXPECT_EQ(Web::CSS::keyword_from_string("smALl"sv).value(), Web::CSS::Keyword::Small);
    EXPECT_EQ(Web::CSS::keyword_from_string(u"INLINE"sv).value(), Web::CSS::Keyword::Inline);
    EXPECT(!Web::CSS::keyword_from_string("not-a-keyword"sv).has_value());
    EXPECT(!Web::CSS::keyword_from_string(u"not-a-keyword"sv).has_value());

    EXPECT_EQ(Web::CSS::property_id_from_string("background-color"sv).value(), Web::CSS::PropertyID::BackgroundColor);
    EXPECT_EQ(Web::CSS::property_id_from_string(u"BACKGROUND-COLOR"sv).value(), Web::CSS::PropertyID::BackgroundColor);
    EXPECT_EQ(Web::CSS::property_id_from_string("--custom"sv).value(), Web::CSS::PropertyID::Custom);
    EXPECT_EQ(Web::CSS::property_id_from_string(u"--custom"sv).value(), Web::CSS::PropertyID::Custom);
    EXPECT(!Web::CSS::property_id_from_string("not-a-property"sv).has_value());
    EXPECT(!Web::CSS::property_id_from_string(u"not-a-property"sv).has_value());
}
