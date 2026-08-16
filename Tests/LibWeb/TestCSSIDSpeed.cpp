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

BENCHMARK_CASE(keyword_from_string)
{
    for (size_t i = 0; i < 10'000'000; ++i) {
        EXPECT_EQ(Web::CSS::keyword_from_string("inline"sv).value(), Web::CSS::Keyword::Inline);
    }
}

BENCHMARK_CASE(keyword_from_utf16_string)
{
    for (size_t i = 0; i < 10'000'000; ++i) {
        EXPECT_EQ(Web::CSS::keyword_from_string(u"inline"sv).value(), Web::CSS::Keyword::Inline);
    }
}

BENCHMARK_CASE(all_properties_from_string)
{
    Vector<String> names;
    for (auto i = to_underlying(Web::CSS::first_property_id); i <= to_underlying(Web::CSS::last_property_id); ++i)
        names.append(MUST(Web::CSS::string_from_property_id(static_cast<Web::CSS::PropertyID>(i)).view().to_utf8()));

    for (size_t iteration = 0; iteration < 20'000; ++iteration) {
        for (auto i = to_underlying(Web::CSS::first_property_id); i <= to_underlying(Web::CSS::last_property_id); ++i) {
            auto property_id = static_cast<Web::CSS::PropertyID>(i);
            EXPECT_EQ(Web::CSS::property_id_from_string(names[i - to_underlying(Web::CSS::first_property_id)]).value(), property_id);
        }
    }
}

BENCHMARK_CASE(all_properties_from_utf16_string)
{
    Vector<Vector<char16_t>> names;
    for (auto i = to_underlying(Web::CSS::first_property_id); i <= to_underlying(Web::CSS::last_property_id); ++i) {
        Vector<char16_t> name;
        auto property_name = Web::CSS::string_from_property_id(static_cast<Web::CSS::PropertyID>(i)).view();
        for (size_t j = 0; j < property_name.length_in_code_units(); ++j)
            name.append(property_name.code_unit_at(j));
        names.append(move(name));
    }

    for (size_t iteration = 0; iteration < 20'000; ++iteration) {
        for (auto i = to_underlying(Web::CSS::first_property_id); i <= to_underlying(Web::CSS::last_property_id); ++i) {
            auto property_id = static_cast<Web::CSS::PropertyID>(i);
            auto const& name = names[i - to_underlying(Web::CSS::first_property_id)];
            EXPECT_EQ(Web::CSS::property_id_from_string(Utf16View { name.data(), name.size() }).value(), property_id);
        }
    }
}
