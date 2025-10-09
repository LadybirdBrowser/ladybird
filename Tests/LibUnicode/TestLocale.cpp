/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibUnicode/Locale.h>

TEST_CASE(is_unicode_language_subtag)
{
    EXPECT(Unicode::is_unicode_language_subtag("aa"sv));
    EXPECT(Unicode::is_unicode_language_subtag("aaa"sv));
    EXPECT(Unicode::is_unicode_language_subtag("aaaaa"sv));
    EXPECT(Unicode::is_unicode_language_subtag("aaaaaa"sv));
    EXPECT(Unicode::is_unicode_language_subtag("aaaaaaa"sv));
    EXPECT(Unicode::is_unicode_language_subtag("aaaaaaaa"sv));

    EXPECT(!Unicode::is_unicode_language_subtag(""sv));
    EXPECT(!Unicode::is_unicode_language_subtag("a"sv));
    EXPECT(!Unicode::is_unicode_language_subtag("aaaa"sv));
    EXPECT(!Unicode::is_unicode_language_subtag("aaaaaaaaa"sv));
    EXPECT(!Unicode::is_unicode_language_subtag("123"sv));
}

TEST_CASE(is_unicode_script_subtag)
{
    EXPECT(Unicode::is_unicode_script_subtag("aaaa"sv));

    EXPECT(!Unicode::is_unicode_script_subtag(""sv));
    EXPECT(!Unicode::is_unicode_script_subtag("a"sv));
    EXPECT(!Unicode::is_unicode_script_subtag("aa"sv));
    EXPECT(!Unicode::is_unicode_script_subtag("aaa"sv));
    EXPECT(!Unicode::is_unicode_script_subtag("aaaaa"sv));
    EXPECT(!Unicode::is_unicode_script_subtag("1234"sv));
}

TEST_CASE(is_unicode_region_subtag)
{
    EXPECT(Unicode::is_unicode_region_subtag("aa"sv));
    EXPECT(Unicode::is_unicode_region_subtag("123"sv));

    EXPECT(!Unicode::is_unicode_region_subtag(""sv));
    EXPECT(!Unicode::is_unicode_region_subtag("a"sv));
    EXPECT(!Unicode::is_unicode_region_subtag("aaa"sv));
    EXPECT(!Unicode::is_unicode_region_subtag("12"sv));
    EXPECT(!Unicode::is_unicode_region_subtag("12a"sv));
}

TEST_CASE(is_unicode_variant_subtag)
{
    EXPECT(Unicode::is_unicode_variant_subtag("aaaaa"sv));
    EXPECT(Unicode::is_unicode_variant_subtag("aaaaaa"sv));
    EXPECT(Unicode::is_unicode_variant_subtag("aaaaaaa"sv));
    EXPECT(Unicode::is_unicode_variant_subtag("aaaaaaaa"sv));

    EXPECT(Unicode::is_unicode_variant_subtag("1aaa"sv));
    EXPECT(Unicode::is_unicode_variant_subtag("12aa"sv));
    EXPECT(Unicode::is_unicode_variant_subtag("123a"sv));
    EXPECT(Unicode::is_unicode_variant_subtag("1234"sv));

    EXPECT(!Unicode::is_unicode_variant_subtag(""sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("a"sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("aa"sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("aaa"sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("aaaa"sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("aaaaaaaaa"sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("a234"sv));
}

TEST_CASE(is_type_identifier)
{
    EXPECT(Unicode::is_type_identifier("aaaa"sv));
    EXPECT(Unicode::is_type_identifier("aaaa-bbbb"sv));
    EXPECT(Unicode::is_type_identifier("aaaa-bbbb-cccc"sv));

    EXPECT(Unicode::is_type_identifier("1aaa"sv));
    EXPECT(Unicode::is_type_identifier("12aa"sv));
    EXPECT(Unicode::is_type_identifier("123a"sv));
    EXPECT(Unicode::is_type_identifier("1234"sv));

    EXPECT(!Unicode::is_type_identifier(""sv));
    EXPECT(!Unicode::is_type_identifier("a"sv));
    EXPECT(!Unicode::is_type_identifier("aa"sv));
    EXPECT(!Unicode::is_type_identifier("aaaaaaaaa"sv));
    EXPECT(!Unicode::is_type_identifier("aaaa-"sv));
}

template<typename LHS, typename RHS>
[[nodiscard]] static bool compare_vectors(LHS const& lhs, RHS const& rhs)
{
    if (lhs.size() != rhs.size())
        return false;

    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i])
            return false;
    }

    return true;
}

TEST_CASE(parse_unicode_locale_id)
{
    auto fail = [](StringView locale) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        EXPECT(!locale_id.has_value());
    };
    auto pass = [](StringView locale, Optional<StringView> expected_language, Optional<StringView> expected_script, Optional<StringView> expected_region, Vector<StringView> const& expected_variants) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        VERIFY(locale_id.has_value());

        EXPECT_EQ(locale_id->language_id.language, expected_language);
        EXPECT_EQ(locale_id->language_id.script, expected_script);
        EXPECT_EQ(locale_id->language_id.region, expected_region);
        EXPECT(compare_vectors(locale_id->language_id.variants, expected_variants));
    };

    fail("a"sv);
    fail("1234"sv);
    fail("aaa-"sv);
    fail("aaa-cc-"sv);
    fail("aaa-bbbb-cc-"sv);
    fail("aaa-bbbb-cc-123"sv);

    pass("aaa"sv, "aaa"sv, {}, {}, {});
    pass("aaa-bbbb"sv, "aaa"sv, "bbbb"sv, {}, {});
    pass("aaa-cc"sv, "aaa"sv, {}, "cc"sv, {});
    pass("aaa-bbbb-cc"sv, "aaa"sv, "bbbb"sv, "cc"sv, {});
    pass("aaa-bbbb-cc-1234"sv, "aaa"sv, "bbbb"sv, "cc"sv, { "1234"sv });
    pass("aaa-bbbb-cc-1234-5678"sv, "aaa"sv, "bbbb"sv, "cc"sv, { "1234"sv, "5678"sv });
}

TEST_CASE(parse_unicode_locale_id_with_unicode_locale_extension)
{
    struct LocaleExtension {
        struct Keyword {
            StringView key {};
            StringView value {};
        };

        Vector<StringView> attributes {};
        Vector<Keyword> keywords {};
    };

    auto fail = [](StringView locale) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        EXPECT(!locale_id.has_value());
    };
    auto pass = [](StringView locale, LocaleExtension const& expected_extension) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        VERIFY(locale_id.has_value());
        EXPECT_EQ(locale_id->extensions.size(), 1u);

        auto const& actual_extension = locale_id->extensions[0].get<Unicode::LocaleExtension>();
        EXPECT(compare_vectors(actual_extension.attributes, expected_extension.attributes));
        EXPECT_EQ(actual_extension.keywords.size(), expected_extension.keywords.size());

        for (size_t i = 0; i < actual_extension.keywords.size(); ++i) {
            auto const& actual_keyword = actual_extension.keywords[i];
            auto const& expected_keyword = expected_extension.keywords[i];

            EXPECT_EQ(actual_keyword.key, expected_keyword.key);
            EXPECT_EQ(actual_keyword.value, expected_keyword.value);
        }
    };

    fail("en-u"sv);
    fail("en-u-"sv);
    fail("en-u-x"sv);
    fail("en-u-xx-"sv);
    fail("en-u--xx"sv);
    fail("en-u-xx-xxxxx-"sv);
    fail("en-u-xx--xxxxx"sv);
    fail("en-u-xx-xxxxxxxxx"sv);
    fail("en-u-xxxxx-"sv);
    fail("en-u-xxxxxxxxx"sv);

    pass("en-u-xx"sv, { {}, { { "xx"sv, ""sv } } });
    pass("en-u-xx-yyyy"sv, { {}, { { "xx"sv, { "yyyy"sv } } } });
    pass("en-u-xx-yyyy-zzzz"sv, { {}, { { "xx"sv, "yyyy-zzzz"sv } } });
    pass("en-u-xx-yyyy-zzzz-aa"sv, { {}, { { "xx"sv, "yyyy-zzzz"sv }, { "aa"sv, ""sv } } });
    pass("en-u-xxx"sv, { { "xxx"sv }, {} });
    pass("en-u-fff-gggg"sv, { { "fff"sv, "gggg"sv }, {} });
    pass("en-u-fff-xx"sv, { { "fff"sv }, { { "xx"sv, ""sv } } });
    pass("en-u-fff-xx-yyyy"sv, { { "fff"sv }, { { "xx"sv, "yyyy"sv } } });
    pass("en-u-fff-gggg-xx-yyyy"sv, { { "fff"sv, "gggg"sv }, { { "xx"sv, "yyyy"sv } } });
}

TEST_CASE(parse_unicode_locale_id_with_transformed_extension)
{
    struct TransformedExtension {
        struct LanguageID {
            bool is_root { false };
            Optional<StringView> language {};
            Optional<StringView> script {};
            Optional<StringView> region {};
            Vector<StringView> variants {};
        };

        struct TransformedField {
            StringView key {};
            StringView value {};
        };

        Optional<LanguageID> language {};
        Vector<TransformedField> fields {};
    };

    auto fail = [](StringView locale) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        EXPECT(!locale_id.has_value());
    };
    auto pass = [](StringView locale, TransformedExtension const& expected_extension) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        VERIFY(locale_id.has_value());
        EXPECT_EQ(locale_id->extensions.size(), 1u);

        auto const& actual_extension = locale_id->extensions[0].get<Unicode::TransformedExtension>();

        VERIFY(actual_extension.language.has_value() == expected_extension.language.has_value());
        if (actual_extension.language.has_value()) {
            EXPECT_EQ(actual_extension.language->language, expected_extension.language->language);
            EXPECT_EQ(actual_extension.language->script, expected_extension.language->script);
            EXPECT_EQ(actual_extension.language->region, expected_extension.language->region);
            EXPECT(compare_vectors(actual_extension.language->variants, expected_extension.language->variants));
        }

        EXPECT_EQ(actual_extension.fields.size(), expected_extension.fields.size());

        for (size_t i = 0; i < actual_extension.fields.size(); ++i) {
            auto const& actual_field = actual_extension.fields[i];
            auto const& expected_field = expected_extension.fields[i];

            EXPECT_EQ(actual_field.key, expected_field.key);
            EXPECT_EQ(actual_field.value, expected_field.value);
        }
    };

    fail("en-t"sv);
    fail("en-t-"sv);
    fail("en-t-a"sv);
    fail("en-t-en-"sv);
    fail("en-t-root"sv);
    fail("en-t-aaaaaaaaa"sv);
    fail("en-t-en-aaa"sv);
    fail("en-t-en-latn-latn"sv);
    fail("en-t-en-a"sv);
    fail("en-t-en-00"sv);
    fail("en-t-en-latn-0"sv);
    fail("en-t-en-latn-00"sv);
    fail("en-t-en-latn-xyz"sv);
    fail("en-t-en-aaaaaaaaa"sv);
    fail("en-t-en-latn-gb-aaaa"sv);
    fail("en-t-en-latn-gb-aaaaaaaaa"sv);
    fail("en-t-k0"sv);
    fail("en-t-k0-aa"sv);
    fail("en-t-k0-aaaaaaaaa"sv);

    pass("en-t-en"sv, { TransformedExtension::LanguageID { false, "en"sv }, {} });
    pass("en-t-en-latn"sv, { TransformedExtension::LanguageID { false, "en"sv, "latn"sv }, {} });
    pass("en-t-en-us"sv, { TransformedExtension::LanguageID { false, "en"sv, {}, "us"sv }, {} });
    pass("en-t-en-latn-us"sv, { TransformedExtension::LanguageID { false, "en"sv, "latn"sv, "us"sv }, {} });
    pass("en-t-en-posix"sv, { TransformedExtension::LanguageID { false, "en"sv, {}, {}, { "posix"sv } }, {} });
    pass("en-t-en-latn-posix"sv, { TransformedExtension::LanguageID { false, "en"sv, "latn"sv, {}, { "posix"sv } }, {} });
    pass("en-t-en-us-posix"sv, { TransformedExtension::LanguageID { false, "en"sv, {}, "us"sv, { "posix"sv } }, {} });
    pass("en-t-en-latn-us-posix"sv, { TransformedExtension::LanguageID { false, "en"sv, "latn"sv, "us"sv, { "posix"sv } }, {} });
    pass("en-t-k0-aaa"sv, { {}, { { "k0"sv, { "aaa"sv } } } });
    pass("en-t-k0-aaa-bbbb"sv, { {}, { { "k0"sv, "aaa-bbbb"sv } } });
    pass("en-t-k0-aaa-k1-bbbb"sv, { {}, { { "k0"sv, { "aaa"sv } }, { "k1"sv, "bbbb"sv } } });
    pass("en-t-en-k0-aaa"sv, { TransformedExtension::LanguageID { false, "en"sv }, { { "k0"sv, "aaa"sv } } });
}

TEST_CASE(parse_unicode_locale_id_with_other_extension)
{
    struct OtherExtension {
        char key {};
        StringView value {};
    };

    auto fail = [](StringView locale) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        EXPECT(!locale_id.has_value());
    };
    auto pass = [](StringView locale, OtherExtension const& expected_extension) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        VERIFY(locale_id.has_value());
        EXPECT_EQ(locale_id->extensions.size(), 1u);

        auto const& actual_extension = locale_id->extensions[0].get<Unicode::OtherExtension>();
        EXPECT_EQ(actual_extension.key, expected_extension.key);
        EXPECT_EQ(actual_extension.value, expected_extension.value);
    };

    fail("en-z"sv);
    fail("en-0"sv);
    fail("en-z-"sv);
    fail("en-0-"sv);
    fail("en-z-a"sv);
    fail("en-0-a"sv);
    fail("en-z-aaaaaaaaa"sv);
    fail("en-0-aaaaaaaaa"sv);
    fail("en-z-aaa-"sv);
    fail("en-0-aaa-"sv);
    fail("en-z-aaa-a"sv);
    fail("en-0-aaa-a"sv);

    pass("en-z-aa"sv, { 'z', "aa"sv });
    pass("en-z-aa-bbb"sv, { 'z', "aa-bbb"sv });
    pass("en-z-aa-bbb-cccccccc"sv, { 'z', "aa-bbb-cccccccc"sv });
}

TEST_CASE(parse_unicode_locale_id_with_private_use_extension)
{
    auto fail = [](StringView locale) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        EXPECT(!locale_id.has_value());
    };
    auto pass = [](StringView locale, Vector<StringView> const& expected_extension) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        VERIFY(locale_id.has_value());
        EXPECT(compare_vectors(locale_id->private_use_extensions, expected_extension));
    };

    fail("en-x"sv);
    fail("en-x-"sv);
    fail("en-x-aaaaaaaaa"sv);
    fail("en-x-aaa-"sv);
    fail("en-x-aaa-aaaaaaaaa"sv);

    pass("en-x-a"sv, { "a"sv });
    pass("en-x-aaaaaaaa"sv, { "aaaaaaaa"sv });
    pass("en-x-aaa-bbb"sv, { "aaa"sv, "bbb"sv });
    pass("en-x-aaa-x-bbb"sv, { "aaa"sv, "x"sv, "bbb"sv });
}

TEST_CASE(canonicalize_unicode_locale_id)
{
    auto test = [](StringView locale, StringView expected_canonical_locale) {
        auto canonical_locale = Unicode::canonicalize_unicode_locale_id(locale);
        EXPECT_EQ(canonical_locale, expected_canonical_locale);
    };

    test("aaa"sv, "aaa"sv);
    test("AaA"sv, "aaa"sv);
    test("aaa-bbbb"sv, "aaa-Bbbb"sv);
    test("aaa-cc"sv, "aaa-CC"sv);
    test("aaa-bBBB-cC"sv, "aaa-Bbbb-CC"sv);
    test("aaa-bbbb-cc-1234"sv, "aaa-Bbbb-CC-1234"sv);
    test("aaa-bbbb-cc-ABCDE"sv, "aaa-Bbbb-CC-abcde"sv);

    test("en-u-aa"sv, "en-u-aa"sv);
    test("EN-U-AA"sv, "en-u-aa"sv);
    test("en-u-aa-bbb"sv, "en-u-aa-bbb"sv);
    test("EN-U-AA-BBB"sv, "en-u-aa-bbb"sv);
    test("en-u-aa-ccc-bbb"sv, "en-u-aa-ccc-bbb"sv);
    test("EN-U-AA-CCC-BBB"sv, "en-u-aa-ccc-bbb"sv);
    test("en-u-ddd-bbb-ccc"sv, "en-u-bbb-ccc-ddd"sv);
    test("EN-U-DDD-BBB-CCC"sv, "en-u-bbb-ccc-ddd"sv);
    test("en-u-2k-aaa-1k-bbb"sv, "en-u-1k-bbb-2k-aaa"sv);
    test("EN-U-2K-AAA-1K-BBB"sv, "en-u-1k-bbb-2k-aaa"sv);
    test("en-u-ccc-bbb-2k-aaa-1k-bbb"sv, "en-u-bbb-ccc-1k-bbb-2k-aaa"sv);
    test("EN-U-CCC-BBB-2K-AAA-1K-BBB"sv, "en-u-bbb-ccc-1k-bbb-2k-aaa"sv);
    test("en-u-1k-true"sv, "en-u-1k"sv);
    test("EN-U-1K-TRUE"sv, "en-u-1k"sv);
    test("en-u-1k-true-abcd"sv, "en-u-1k-true-abcd"sv);
    test("EN-U-1K-TRUE-ABCD"sv, "en-u-1k-true-abcd"sv);
    test("en-u-kb-yes"sv, "en-u-kb"sv);
    test("EN-U-KB-YES"sv, "en-u-kb"sv);
    test("en-u-kb-yes-abcd"sv, "en-u-kb-yes-abcd"sv);
    test("EN-U-KB-YES-ABCD"sv, "en-u-kb-yes-abcd"sv);
    test("en-u-ka-yes"sv, "en-u-ka"sv);
    test("EN-U-KA-YES"sv, "en-u-ka"sv);
    test("en-u-1k-names"sv, "en-u-1k-names"sv);
    test("EN-U-1K-NAMES"sv, "en-u-1k-names"sv);
    test("en-u-ks-primary"sv, "en-u-ks-level1"sv);
    test("EN-U-KS-PRIMARY"sv, "en-u-ks-level1"sv);
    test("en-u-ka-primary"sv, "en-u-ka-primary"sv);
    test("EN-U-KA-PRIMARY"sv, "en-u-ka-primary"sv);
    test("en-u-ms-imperial"sv, "en-u-ms-uksystem"sv);
    test("EN-U-MS-IMPERIAL"sv, "en-u-ms-uksystem"sv);
    test("en-u-ma-imperial"sv, "en-u-ma-imperial"sv);
    test("EN-U-MA-IMPERIAL"sv, "en-u-ma-imperial"sv);
    test("en-u-tz-hongkong"sv, "en-u-tz-hkhkg"sv);
    test("EN-U-TZ-HONGKONG"sv, "en-u-tz-hkhkg"sv);
    test("en-u-ta-hongkong"sv, "en-u-ta-hongkong"sv);
    test("EN-U-TA-HONGKONG"sv, "en-u-ta-hongkong"sv);
    test("en-u-ca-ethiopic-amete-alem"sv, "en-u-ca-ethioaa"sv);
    test("EN-U-CA-ETHIOPIC-AMETE-ALEM"sv, "en-u-ca-ethioaa"sv);
    test("en-u-ca-alem-ethiopic-amete"sv, "en-u-ca-alem-ethiopic-amete"sv);
    test("EN-U-CA-ALEM-ETHIOPIC-AMETE"sv, "en-u-ca-alem-ethiopic-amete"sv);
    test("en-u-ca-ethiopic-amete-xxx-alem"sv, "en-u-ca-ethiopic-amete-xxx-alem"sv);
    test("EN-U-CA-ETHIOPIC-AMETE-XXX-ALEM"sv, "en-u-ca-ethiopic-amete-xxx-alem"sv);
    test("en-u-cb-ethiopic-amete-alem"sv, "en-u-cb-ethiopic-amete-alem"sv);
    test("EN-U-CB-ETHIOPIC-AMETE-ALEM"sv, "en-u-cb-ethiopic-amete-alem"sv);

    test("en-t-en"sv, "en-t-en"sv);
    test("EN-T-EN"sv, "en-t-en"sv);
    test("en-latn-t-en-latn"sv, "en-Latn-t-en-latn"sv);
    test("EN-LATN-T-EN-LATN"sv, "en-Latn-t-en-latn"sv);
    test("en-us-t-en-us"sv, "en-US-t-en-us"sv);
    test("EN-US-T-EN-US"sv, "en-US-t-en-us"sv);
    test("en-latn-us-t-en-latn-us"sv, "en-Latn-US-t-en-latn-us"sv);
    test("EN-LATN-US-T-EN-LATN-US"sv, "en-Latn-US-t-en-latn-us"sv);
    test("en-t-en-k2-bbb-k1-aaa"sv, "en-t-en-k1-aaa-k2-bbb"sv);
    test("EN-T-EN-K2-BBB-K1-AAA"sv, "en-t-en-k1-aaa-k2-bbb"sv);
    test("en-t-k1-true"sv, "en-t-k1-true"sv);
    test("EN-T-K1-TRUE"sv, "en-t-k1-true"sv);
    test("en-t-k1-yes"sv, "en-t-k1-yes"sv);
    test("EN-T-K1-YES"sv, "en-t-k1-yes"sv);
    test("en-t-m0-names"sv, "en-t-m0-prprname"sv);
    test("EN-T-M0-NAMES"sv, "en-t-m0-prprname"sv);
    test("en-t-k1-names"sv, "en-t-k1-names"sv);
    test("EN-T-K1-NAMES"sv, "en-t-k1-names"sv);
    test("en-t-k1-primary"sv, "en-t-k1-primary"sv);
    test("EN-T-K1-PRIMARY"sv, "en-t-k1-primary"sv);
    test("en-t-k1-imperial"sv, "en-t-k1-imperial"sv);
    test("EN-T-K1-IMPERIAL"sv, "en-t-k1-imperial"sv);
    test("en-t-k1-hongkong"sv, "en-t-k1-hongkong"sv);
    test("EN-T-K1-HONGKONG"sv, "en-t-k1-hongkong"sv);
    test("en-t-k1-ethiopic-amete-alem"sv, "en-t-k1-ethiopic-amete-alem"sv);
    test("EN-T-K1-ETHIOPIC-AMETE-ALEM"sv, "en-t-k1-ethiopic-amete-alem"sv);

    test("en-0-aaa"sv, "en-0-aaa"sv);
    test("EN-0-AAA"sv, "en-0-aaa"sv);
    test("en-0-bbb-aaa"sv, "en-0-bbb-aaa"sv);
    test("EN-0-BBB-AAA"sv, "en-0-bbb-aaa"sv);
    test("en-z-bbb-0-aaa"sv, "en-0-aaa-z-bbb"sv);
    test("EN-Z-BBB-0-AAA"sv, "en-0-aaa-z-bbb"sv);

    test("en-x-aa"sv, "en-x-aa"sv);
    test("EN-X-AA"sv, "en-x-aa"sv);
    test("en-x-bbb-aa"sv, "en-x-bbb-aa"sv);
    test("EN-X-BBB-AA"sv, "en-x-bbb-aa"sv);

    test("en-u-aa-t-en"sv, "en-t-en-u-aa"sv);
    test("EN-U-AA-T-EN"sv, "en-t-en-u-aa"sv);
    test("en-z-bbb-u-aa-t-en-0-aaa"sv, "en-0-aaa-t-en-u-aa-z-bbb"sv);
    test("EN-Z-BBB-U-AA-T-EN-0-AAA"sv, "en-0-aaa-t-en-u-aa-z-bbb"sv);
    test("en-z-bbb-u-aa-t-en-0-aaa-x-ccc"sv, "en-0-aaa-t-en-u-aa-z-bbb-x-ccc"sv);
    test("EN-Z-BBB-U-AA-T-EN-0-AAA-X-CCC"sv, "en-0-aaa-t-en-u-aa-z-bbb-x-ccc"sv);

    // Language subtag aliases.
    test("sh"sv, "sr-Latn"sv);
    test("SH"sv, "sr-Latn"sv);
    test("sh-cyrl"sv, "sr-Cyrl"sv);
    test("SH-CYRL"sv, "sr-Cyrl"sv);
    test("cnr"sv, "sr-ME"sv);
    test("CNR"sv, "sr-ME"sv);
    test("cnr-ba"sv, "sr-BA"sv);
    test("CNR-BA"sv, "sr-BA"sv);

    // Territory subtag aliases.
    test("ru-su"sv, "ru-RU"sv);
    test("RU-SU"sv, "ru-RU"sv);
    test("ru-810"sv, "ru-RU"sv);
    test("RU-810"sv, "ru-RU"sv);
    test("en-su"sv, "en-RU"sv);
    test("EN-SU"sv, "en-RU"sv);
    test("en-810"sv, "en-RU"sv);
    test("EN-810"sv, "en-RU"sv);
    test("hy-su"sv, "hy-AM"sv);
    test("HY-SU"sv, "hy-AM"sv);
    test("hy-810"sv, "hy-AM"sv);
    test("HY-810"sv, "hy-AM"sv);
    test("und-Armn-su"sv, "und-Armn-AM"sv);
    test("UND-ARMN-SU"sv, "und-Armn-AM"sv);
    test("und-Armn-810"sv, "und-Armn-AM"sv);
    test("UND-ARMN-810"sv, "und-Armn-AM"sv);

    // Script subtag aliases.
    test("en-qaai"sv, "en-Zinh"sv);
    test("EN-QAAI"sv, "en-Zinh"sv);

    // Variant subtag aliases.
    test("en-polytoni"sv, "en-polyton"sv);
    test("EN-POLYTONI"sv, "en-polyton"sv);

    // Subdivision subtag aliases.
    test("en-u-sd-cn11"sv, "en-u-sd-cnbj"sv);
    test("EN-U-SD-CN11"sv, "en-u-sd-cnbj"sv);
    test("en-u-rg-cn12"sv, "en-u-rg-cntj"sv);
    test("EN-U-RG-CN12"sv, "en-u-rg-cntj"sv);
    test("en-u-aa-cn11"sv, "en-u-aa-cn11"sv);
    test("EN-U-AA-CN11"sv, "en-u-aa-cn11"sv);

    // Complex aliases.
    test("en-lojban"sv, "en"sv);
    test("EN-LOJBAN"sv, "en"sv);
    test("art-lojban"sv, "jbo"sv);
    test("ART-LOJBAN"sv, "jbo"sv);
    test("cel-gaulish"sv, "xtg"sv);
    test("CEL-GAULISH"sv, "xtg"sv);
    test("zh-guoyu"sv, "zh"sv);
    test("ZH-GUOYU"sv, "zh"sv);
    test("zh-hakka"sv, "hak"sv);
    test("ZH-HAKKA"sv, "hak"sv);
    test("zh-xiang"sv, "hsn"sv);
    test("ZH-XIANG"sv, "hsn"sv);
    test("ja-latn-hepburn-heploc"sv, "ja-Latn-alalc97"sv);
    test("JA-LATN-HEPBURN-HEPLOC"sv, "ja-Latn-alalc97"sv);

    // Default content.
    test("en-us"sv, "en-US"sv);
    test("EN-US"sv, "en-US"sv);
    test("zh-Hans-CN"sv, "zh-Hans-CN"sv);
    test("ZH-HANS-CN"sv, "zh-Hans-CN"sv);
}

TEST_CASE(supports_locale_aliases)
{
    EXPECT(Unicode::is_locale_available("zh"sv));
    EXPECT(Unicode::is_locale_available("zh-Hant"sv));
    EXPECT(Unicode::is_locale_available("zh-TW"sv));
    EXPECT(Unicode::is_locale_available("zh-Hant-TW"sv));
}
