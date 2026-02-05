/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/StringView.h>
#include <LibUnicode/CharacterTypes.h>

TEST_CASE(general_category)
{
    auto general_category = [](StringView name) {
        auto general_category = Unicode::general_category_from_string(name);
        VERIFY(general_category.has_value());
        return *general_category;
    };

    auto general_category_c = general_category("C"sv);
    auto general_category_other = general_category("Other"sv);
    EXPECT_EQ(general_category_c, general_category_other);

    auto general_category_cc = general_category("Cc"sv);
    auto general_category_control = general_category("Control"sv);
    EXPECT_EQ(general_category_cc, general_category_control);

    auto general_category_co = general_category("Co"sv);
    auto general_category_private_use = general_category("Private_Use"sv);
    EXPECT_EQ(general_category_co, general_category_private_use);

    auto general_category_cn = general_category("Cn"sv);
    auto general_category_unassigned = general_category("Unassigned"sv);
    EXPECT_EQ(general_category_cn, general_category_unassigned);

    auto general_category_lc = general_category("LC"sv);
    auto general_category_cased_letter = general_category("Cased_Letter"sv);
    EXPECT_EQ(general_category_lc, general_category_cased_letter);

    auto general_category_ll = general_category("Ll"sv);
    auto general_category_lowercase_letter = general_category("Lowercase_Letter"sv);
    EXPECT_EQ(general_category_ll, general_category_lowercase_letter);

    auto general_category_lu = general_category("Lu"sv);
    auto general_category_uppercase_letter = general_category("Uppercase_Letter"sv);
    EXPECT_EQ(general_category_lu, general_category_uppercase_letter);

    for (u32 code_point = 0; code_point <= 0x1f; ++code_point) {
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_c));
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_cc));

        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_co));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_cn));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_lc));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_ll));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_lu));
    }

    for (u32 code_point = 0xe000; code_point <= 0xe100; ++code_point) {
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_c));
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_co));

        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_cc));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_cn));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_lc));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_ll));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_lu));
    }

    for (u32 code_point = 0x101fe; code_point <= 0x1027f; ++code_point) {
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_c));
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_cn));

        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_cc));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_co));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_lc));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_ll));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_lu));
    }

    for (u32 code_point = 0x61; code_point <= 0x7a; ++code_point) {
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_lc));
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_ll));

        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_c));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_cc));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_co));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_cn));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_lu));
    }

    for (u32 code_point = 0x41; code_point <= 0x5a; ++code_point) {
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_lc));
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_lu));

        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_c));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_cc));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_co));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_cn));
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_ll));
    }
}

BENCHMARK_CASE(general_category_performance)
{
    auto general_category_cased_letter = Unicode::general_category_from_string("Cased_Letter"sv).value();

    for (size_t i = 0; i < 1'000'000; ++i) {
        for (u32 code_point = 0; code_point <= 0x1f; ++code_point)
            EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_cased_letter));

        for (u32 code_point = 0x41; code_point <= 0x5a; ++code_point)
            EXPECT(Unicode::code_point_has_general_category(code_point, general_category_cased_letter));

        for (u32 code_point = 0x61; code_point <= 0x7a; ++code_point)
            EXPECT(Unicode::code_point_has_general_category(code_point, general_category_cased_letter));

        for (u32 code_point = 0xe000; code_point <= 0xe100; ++code_point)
            EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_cased_letter));

        for (u32 code_point = 0x101fe; code_point <= 0x1027f; ++code_point)
            EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_cased_letter));
    }
}

TEST_CASE(property)
{
    auto property = [](StringView name) {
        auto property = Unicode::property_from_string(name);
        VERIFY(property.has_value());
        return *property;
    };

    auto property_any = property("Any"sv);
    auto property_assigned = property("Assigned"sv);
    auto property_ascii = property("ASCII"sv);

    auto property_white_space = property("White_Space"sv);
    auto property_wspace = property("WSpace"sv);
    auto property_space = property("space"sv);
    EXPECT_EQ(property_white_space, property_wspace);
    EXPECT_EQ(property_white_space, property_space);

    auto property_emoji_presentation = property("Emoji_Presentation"sv);
    auto property_epres = property("EPres"sv);
    EXPECT_EQ(property_emoji_presentation, property_epres);

    for (u32 code_point = 0; code_point <= 0x10ffff; code_point += 1000)
        EXPECT(Unicode::code_point_has_property(code_point, property_any));

    for (u32 code_point = 0x101d0; code_point <= 0x101fd; ++code_point) {
        EXPECT(Unicode::code_point_has_property(code_point, property_any));
        EXPECT(Unicode::code_point_has_property(code_point, property_assigned));

        EXPECT(!Unicode::code_point_has_property(code_point, property_ascii));
        EXPECT(!Unicode::code_point_has_property(code_point, property_white_space));
        EXPECT(!Unicode::code_point_has_property(code_point, property_emoji_presentation));
    }

    for (u32 code_point = 0x101fe; code_point <= 0x1027f; ++code_point) {
        EXPECT(Unicode::code_point_has_property(code_point, property_any));

        EXPECT(!Unicode::code_point_has_property(code_point, property_assigned));
        EXPECT(!Unicode::code_point_has_property(code_point, property_ascii));
        EXPECT(!Unicode::code_point_has_property(code_point, property_white_space));
        EXPECT(!Unicode::code_point_has_property(code_point, property_emoji_presentation));
    }

    for (u32 code_point = 0; code_point <= 0x7f; ++code_point) {
        EXPECT(Unicode::code_point_has_property(code_point, property_any));
        EXPECT(Unicode::code_point_has_property(code_point, property_assigned));
        EXPECT(Unicode::code_point_has_property(code_point, property_ascii));

        EXPECT(!Unicode::code_point_has_property(code_point, property_emoji_presentation));
    }

    for (u32 code_point = 0x9; code_point <= 0xd; ++code_point) {
        EXPECT(Unicode::code_point_has_property(code_point, property_any));
        EXPECT(Unicode::code_point_has_property(code_point, property_assigned));
        EXPECT(Unicode::code_point_has_property(code_point, property_ascii));
        EXPECT(Unicode::code_point_has_property(code_point, property_white_space));

        EXPECT(!Unicode::code_point_has_property(code_point, property_emoji_presentation));
    }

    for (u32 code_point = 0x1f3e5; code_point <= 0x1f3f0; ++code_point) {
        EXPECT(Unicode::code_point_has_property(code_point, property_any));
        EXPECT(Unicode::code_point_has_property(code_point, property_assigned));
        EXPECT(Unicode::code_point_has_property(code_point, property_emoji_presentation));

        EXPECT(!Unicode::code_point_has_property(code_point, property_ascii));
        EXPECT(!Unicode::code_point_has_property(code_point, property_white_space));
    }
}

TEST_CASE(script)
{
    auto script = [](StringView name) {
        auto script = Unicode::script_from_string(name);
        VERIFY(script.has_value());
        return *script;
    };

    auto script_latin = script("Latin"sv);
    auto script_latn = script("Latn"sv);
    EXPECT_EQ(script_latin, script_latn);

    auto script_cyrillic = script("Cyrillic"sv);
    auto script_cyrl = script("Cyrl"sv);
    EXPECT_EQ(script_cyrillic, script_cyrl);

    auto script_greek = script("Greek"sv);
    auto script_grek = script("Grek"sv);
    EXPECT_EQ(script_greek, script_grek);

    for (u32 code_point = 0x41; code_point <= 0x5a; ++code_point) {
        EXPECT(Unicode::code_point_has_script(code_point, script_latin));
        EXPECT(Unicode::code_point_has_script_extension(code_point, script_latin));

        EXPECT(!Unicode::code_point_has_script(code_point, script_cyrillic));
        EXPECT(!Unicode::code_point_has_script(code_point, script_greek));
    }

    for (u32 code_point = 0x61; code_point <= 0x7a; ++code_point) {
        EXPECT(Unicode::code_point_has_script(code_point, script_latin));
        EXPECT(Unicode::code_point_has_script_extension(code_point, script_latin));

        EXPECT(!Unicode::code_point_has_script(code_point, script_cyrillic));
        EXPECT(!Unicode::code_point_has_script(code_point, script_greek));
    }

    for (u32 code_point = 0x400; code_point <= 0x481; ++code_point) {
        EXPECT(Unicode::code_point_has_script(code_point, script_cyrillic));
        EXPECT(Unicode::code_point_has_script_extension(code_point, script_cyrillic));

        EXPECT(!Unicode::code_point_has_script(code_point, script_latin));
        EXPECT(!Unicode::code_point_has_script(code_point, script_greek));
    }

    for (u32 code_point = 0x1f80; code_point <= 0x1fb4; ++code_point) {
        EXPECT(Unicode::code_point_has_script(code_point, script_greek));
        EXPECT(Unicode::code_point_has_script_extension(code_point, script_greek));

        EXPECT(!Unicode::code_point_has_script(code_point, script_latin));
        EXPECT(!Unicode::code_point_has_script(code_point, script_cyrillic));
    }
}

TEST_CASE(script_extension)
{
    auto script = [](StringView name) {
        auto script = Unicode::script_from_string(name);
        VERIFY(script.has_value());
        return *script;
    };

    auto script_latin = script("Latin"sv);
    auto script_greek = script("Greek"sv);

    for (u32 code_point = 0x363; code_point <= 0x36f; ++code_point) {
        EXPECT(!Unicode::code_point_has_script(code_point, script_latin));
        EXPECT(Unicode::code_point_has_script_extension(code_point, script_latin));
    }

    EXPECT(!Unicode::code_point_has_script(0x342, script_greek));
    EXPECT(Unicode::code_point_has_script_extension(0x342, script_greek));

    EXPECT(!Unicode::code_point_has_script(0x345, script_greek));
    EXPECT(Unicode::code_point_has_script_extension(0x345, script_greek));

    EXPECT(!Unicode::code_point_has_script(0x1dc0, script_greek));
    EXPECT(Unicode::code_point_has_script_extension(0x1dc0, script_greek));

    EXPECT(!Unicode::code_point_has_script(0x1dc1, script_greek));
    EXPECT(Unicode::code_point_has_script_extension(0x1dc1, script_greek));

    auto script_common = script("Common"sv);
    auto script_zyyy = script("Zyyy"sv);
    EXPECT_EQ(script_common, script_zyyy);

    EXPECT(Unicode::code_point_has_script(0x202f, script_common));
    EXPECT(!Unicode::code_point_has_script_extension(0x202f, script_common));

    EXPECT(Unicode::code_point_has_script(0x3000, script_common));
    EXPECT(Unicode::code_point_has_script_extension(0x3000, script_common));

    auto script_inherited = script("Inherited"sv);
    auto script_qaai = script("Qaai"sv);
    auto script_zinh = script("Zinh"sv);
    EXPECT_EQ(script_inherited, script_qaai);
    EXPECT_EQ(script_inherited, script_zinh);

    EXPECT(Unicode::code_point_has_script(0x1ced, script_inherited));
    EXPECT(!Unicode::code_point_has_script_extension(0x1ced, script_inherited));

    EXPECT(Unicode::code_point_has_script(0x101fd, script_inherited));
    EXPECT(Unicode::code_point_has_script_extension(0x101fd, script_inherited));
}

TEST_CASE(code_point_bidirectional_character_type)
{
    // Left-to-right
    EXPECT_EQ(Unicode::bidirectional_class('A'), Unicode::BidiClass::LeftToRight);
    EXPECT_EQ(Unicode::bidirectional_class('z'), Unicode::BidiClass::LeftToRight);
    // European number
    EXPECT_EQ(Unicode::bidirectional_class('7'), Unicode::BidiClass::EuropeanNumber);
    // Whitespace
    EXPECT_EQ(Unicode::bidirectional_class(' '), Unicode::BidiClass::WhiteSpaceNeutral);
    // Arabic right-to-left (U+FEB4 ARABIC LETTER SEEN MEDIAL FORM)
    EXPECT_EQ(Unicode::bidirectional_class(0xFEB4), Unicode::BidiClass::RightToLeftArabic);
}
