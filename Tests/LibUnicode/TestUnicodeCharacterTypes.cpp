/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/StringView.h>
#include <AK/Utf16View.h>
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

    for (u32 code_point = 0x61; code_point <= 0x7a; ++code_point) {
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_lu, CaseSensitivity::CaseSensitive));
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_lu, CaseSensitivity::CaseInsensitive));
    }

    for (u32 code_point = 0x41; code_point <= 0x5a; ++code_point) {
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_ll, CaseSensitivity::CaseSensitive));
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_ll, CaseSensitivity::CaseInsensitive));
    }

    for (u32 code_point = 0x0410; code_point <= 0x042F; ++code_point) {
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_ll, CaseSensitivity::CaseSensitive));
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_ll, CaseSensitivity::CaseInsensitive));
    }

    for (u32 code_point = 0x0430; code_point <= 0x044F; ++code_point) {
        EXPECT(!Unicode::code_point_has_general_category(code_point, general_category_lu, CaseSensitivity::CaseSensitive));
        EXPECT(Unicode::code_point_has_general_category(code_point, general_category_lu, CaseSensitivity::CaseInsensitive));
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
    auto property_uppercase = property("Uppercase"sv);
    auto property_lowercase = property("Lowercase"sv);

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

    for (u32 code_point = 0x61; code_point <= 0x7a; ++code_point) {
        EXPECT(!Unicode::code_point_has_property(code_point, property_uppercase, CaseSensitivity::CaseSensitive));
        EXPECT(Unicode::code_point_has_property(code_point, property_uppercase, CaseSensitivity::CaseInsensitive));
    }

    for (u32 code_point = 0x41; code_point <= 0x5a; ++code_point) {
        EXPECT(!Unicode::code_point_has_property(code_point, property_lowercase, CaseSensitivity::CaseSensitive));
        EXPECT(Unicode::code_point_has_property(code_point, property_lowercase, CaseSensitivity::CaseInsensitive));
    }

    for (u32 code_point = 0x0430; code_point <= 0x044F; ++code_point) {
        EXPECT(!Unicode::code_point_has_property(code_point, property_uppercase, CaseSensitivity::CaseSensitive));
        EXPECT(Unicode::code_point_has_property(code_point, property_uppercase, CaseSensitivity::CaseInsensitive));
    }

    for (u32 code_point = 0x0410; code_point <= 0x042F; ++code_point) {
        EXPECT(!Unicode::code_point_has_property(code_point, property_lowercase, CaseSensitivity::CaseSensitive));
        EXPECT(Unicode::code_point_has_property(code_point, property_lowercase, CaseSensitivity::CaseInsensitive));
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

TEST_CASE(canonicalize)
{
    constexpr u32 LATIN_CAPITAL_A_GRAVE = 0x00C0;   // À
    constexpr u32 LATIN_SMALL_A_GRAVE = 0x00E0;     // à
    constexpr u32 LATIN_CAPITAL_SHARP_S = 0x1E9E;   // ẞ
    constexpr u32 LATIN_SMALL_SHARP_S = 0x00DF;     // ß
    constexpr u32 LATIN_CAPITAL_OE = 0x0152;        // Œ
    constexpr u32 LATIN_SMALL_OE = 0x0153;          // œ
    constexpr u32 GREEK_CAPITAL_SIGMA = 0x03A3;     // Σ
    constexpr u32 GREEK_SMALL_SIGMA = 0x03C3;       // σ
    constexpr u32 GREEK_SMALL_FINAL_SIGMA = 0x03C2; // ς
    constexpr u32 KELVIN_SIGN = 0x212A;             // K

    EXPECT_EQ(Unicode::canonicalize('A', true), static_cast<u32>('a'));
    EXPECT_EQ(Unicode::canonicalize('a', false), static_cast<u32>('A'));

    EXPECT_EQ(Unicode::canonicalize(KELVIN_SIGN, true), static_cast<u32>('k'));
    EXPECT_EQ(Unicode::canonicalize(KELVIN_SIGN, false), KELVIN_SIGN);

    EXPECT_EQ(Unicode::canonicalize(LATIN_CAPITAL_A_GRAVE, true), LATIN_SMALL_A_GRAVE);
    EXPECT_EQ(Unicode::canonicalize(LATIN_SMALL_A_GRAVE, false), LATIN_CAPITAL_A_GRAVE);

    EXPECT_EQ(Unicode::canonicalize(LATIN_CAPITAL_SHARP_S, true), LATIN_SMALL_SHARP_S);
    EXPECT_EQ(Unicode::canonicalize(LATIN_SMALL_SHARP_S, false), LATIN_SMALL_SHARP_S);

    EXPECT_EQ(Unicode::canonicalize(GREEK_CAPITAL_SIGMA, true), GREEK_SMALL_SIGMA);
    EXPECT_EQ(Unicode::canonicalize(GREEK_SMALL_FINAL_SIGMA, true), GREEK_SMALL_SIGMA);

    EXPECT_EQ(Unicode::canonicalize(LATIN_CAPITAL_OE, true), LATIN_SMALL_OE);
    EXPECT_EQ(Unicode::canonicalize(LATIN_SMALL_OE, false), LATIN_CAPITAL_OE);
}

TEST_CASE(expand_range_case_insensitive)
{
    auto latin_ranges = Unicode::expand_range_case_insensitive('a', 'z');
    EXPECT_EQ(latin_ranges.size(), 4uz);

    EXPECT(any_of(latin_ranges, [](auto const& range) {
        return range.from == 'a' && range.to == 'z';
    }));

    EXPECT(any_of(latin_ranges, [](auto const& range) {
        return range.from == 'A' && range.to == 'Z';
    }));

    // LATIN SMALL LETTER LONG S (ſ)
    EXPECT(any_of(latin_ranges, [](auto const& range) {
        return range.from == 0x017F && range.to == 0x017F;
    }));

    // KELVIN SIGN (K)
    EXPECT(any_of(latin_ranges, [](auto const& range) {
        return range.from == 0x212A && range.to == 0x212A;
    }));

    auto k_ranges = Unicode::expand_range_case_insensitive('k', 'k');
    EXPECT_EQ(k_ranges.size(), 3uz);

    // KELVIN SIGN (K)
    EXPECT(any_of(k_ranges, [](auto const& range) {
        return range.from == 0x212A && range.to == 0x212A;
    }));
}

TEST_CASE(for_each_case_folded_code_point)
{
    constexpr u32 GREEK_SMALL_SIGMA = 0x03C3;       // σ
    constexpr u32 GREEK_SMALL_FINAL_SIGMA = 0x03C2; // ς
    constexpr u32 GREEK_CAPITAL_SIGMA = 0x03A3;     // Σ
    constexpr u32 KELVIN_SIGN = 0x212A;             // K

    Vector<u32> folded_A;
    Unicode::for_each_case_folded_code_point('A', [&](u32 cp) {
        folded_A.append(cp);
        return IterationDecision::Continue;
    });
    EXPECT(folded_A.contains_slow('A'));
    EXPECT(folded_A.contains_slow('a'));

    Vector<u32> folded_sigma;
    Unicode::for_each_case_folded_code_point(GREEK_CAPITAL_SIGMA, [&](u32 cp) {
        folded_sigma.append(cp);
        return IterationDecision::Continue;
    });
    EXPECT(folded_sigma.contains_slow(GREEK_CAPITAL_SIGMA));
    EXPECT(folded_sigma.contains_slow(GREEK_SMALL_SIGMA));
    EXPECT(folded_sigma.contains_slow(GREEK_SMALL_FINAL_SIGMA));

    Vector<u32> folded_kelvin;
    Unicode::for_each_case_folded_code_point(KELVIN_SIGN, [&](u32 cp) {
        folded_kelvin.append(cp);
        return IterationDecision::Continue;
    });
    EXPECT(folded_kelvin.contains_slow(KELVIN_SIGN));
    EXPECT(folded_kelvin.contains_slow('K'));
    EXPECT(folded_kelvin.contains_slow('k'));
}

TEST_CASE(code_point_matches_range_ignoring_case)
{
    constexpr u32 LATIN_CAPITAL_A_GRAVE = 0x00C0;   // À
    constexpr u32 LATIN_SMALL_A_GRAVE = 0x00E0;     // à
    constexpr u32 GREEK_SMALL_SIGMA = 0x03C3;       // σ
    constexpr u32 GREEK_SMALL_FINAL_SIGMA = 0x03C2; // ς
    constexpr u32 MICRO_SIGN = 0x00B5;              // µ
    constexpr u32 GREEK_SMALL_MU = 0x03BC;          // μ
    constexpr u32 KELVIN_SIGN = 0x212A;             // K

    EXPECT(Unicode::code_point_matches_range_ignoring_case('B', 'a', 'z', true));
    EXPECT(Unicode::code_point_matches_range_ignoring_case('b', 'A', 'Z', true));

    EXPECT(Unicode::code_point_matches_range_ignoring_case(KELVIN_SIGN, 'a', 'z', true));
    EXPECT(!Unicode::code_point_matches_range_ignoring_case(KELVIN_SIGN, 'a', 'z', false));

    EXPECT(Unicode::code_point_matches_range_ignoring_case(LATIN_SMALL_A_GRAVE, LATIN_CAPITAL_A_GRAVE, LATIN_CAPITAL_A_GRAVE, true));
    EXPECT(Unicode::code_point_matches_range_ignoring_case(LATIN_CAPITAL_A_GRAVE, LATIN_SMALL_A_GRAVE, LATIN_SMALL_A_GRAVE, true));

    EXPECT(Unicode::code_point_matches_range_ignoring_case(GREEK_SMALL_FINAL_SIGMA, GREEK_SMALL_SIGMA, GREEK_SMALL_SIGMA, true));
    EXPECT(Unicode::code_point_matches_range_ignoring_case(MICRO_SIGN, GREEK_SMALL_MU, GREEK_SMALL_MU, true));
}

TEST_CASE(ranges_equal_ignoring_case)
{
    EXPECT(Unicode::ranges_equal_ignoring_case(Utf8View("Hello"sv), Utf16View("HELLO"sv), true));
    EXPECT(Unicode::ranges_equal_ignoring_case(Utf16View("Hello"sv), Utf8View("hello"sv), true));

    EXPECT(Unicode::ranges_equal_ignoring_case(Utf8View("Σσς"sv), Utf8View("ΣΣΣ"sv), true));
    EXPECT(Unicode::ranges_equal_ignoring_case(Utf8View("straße"sv), Utf8View("STRAẞE"sv), true));
    EXPECT(Unicode::ranges_equal_ignoring_case(Utf8View("CAFÉ"sv), Utf8View("café"sv), true));
    EXPECT(Unicode::ranges_equal_ignoring_case(Utf8View("Œ"sv), Utf8View("œ"sv), true));

    EXPECT(Unicode::ranges_equal_ignoring_case(Utf8View("K"sv), Utf8View("K"sv), true));
    EXPECT(!Unicode::ranges_equal_ignoring_case(Utf8View("K"sv), Utf8View("K"sv), false));
}
