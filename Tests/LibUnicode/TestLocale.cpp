/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibUnicode/Locale.h>

TEST_CASE(is_unicode_language_subtag)
{
    EXPECT(Unicode::is_unicode_language_subtag("aa"_sv));
    EXPECT(Unicode::is_unicode_language_subtag("aaa"_sv));
    EXPECT(Unicode::is_unicode_language_subtag("aaaaa"_sv));
    EXPECT(Unicode::is_unicode_language_subtag("aaaaaa"_sv));
    EXPECT(Unicode::is_unicode_language_subtag("aaaaaaa"_sv));
    EXPECT(Unicode::is_unicode_language_subtag("aaaaaaaa"_sv));

    EXPECT(!Unicode::is_unicode_language_subtag(""_sv));
    EXPECT(!Unicode::is_unicode_language_subtag("a"_sv));
    EXPECT(!Unicode::is_unicode_language_subtag("aaaa"_sv));
    EXPECT(!Unicode::is_unicode_language_subtag("aaaaaaaaa"_sv));
    EXPECT(!Unicode::is_unicode_language_subtag("123"_sv));
}

TEST_CASE(is_unicode_script_subtag)
{
    EXPECT(Unicode::is_unicode_script_subtag("aaaa"_sv));

    EXPECT(!Unicode::is_unicode_script_subtag(""_sv));
    EXPECT(!Unicode::is_unicode_script_subtag("a"_sv));
    EXPECT(!Unicode::is_unicode_script_subtag("aa"_sv));
    EXPECT(!Unicode::is_unicode_script_subtag("aaa"_sv));
    EXPECT(!Unicode::is_unicode_script_subtag("aaaaa"_sv));
    EXPECT(!Unicode::is_unicode_script_subtag("1234"_sv));
}

TEST_CASE(is_unicode_region_subtag)
{
    EXPECT(Unicode::is_unicode_region_subtag("aa"_sv));
    EXPECT(Unicode::is_unicode_region_subtag("123"_sv));

    EXPECT(!Unicode::is_unicode_region_subtag(""_sv));
    EXPECT(!Unicode::is_unicode_region_subtag("a"_sv));
    EXPECT(!Unicode::is_unicode_region_subtag("aaa"_sv));
    EXPECT(!Unicode::is_unicode_region_subtag("12"_sv));
    EXPECT(!Unicode::is_unicode_region_subtag("12a"_sv));
}

TEST_CASE(is_unicode_variant_subtag)
{
    EXPECT(Unicode::is_unicode_variant_subtag("aaaaa"_sv));
    EXPECT(Unicode::is_unicode_variant_subtag("aaaaaa"_sv));
    EXPECT(Unicode::is_unicode_variant_subtag("aaaaaaa"_sv));
    EXPECT(Unicode::is_unicode_variant_subtag("aaaaaaaa"_sv));

    EXPECT(Unicode::is_unicode_variant_subtag("1aaa"_sv));
    EXPECT(Unicode::is_unicode_variant_subtag("12aa"_sv));
    EXPECT(Unicode::is_unicode_variant_subtag("123a"_sv));
    EXPECT(Unicode::is_unicode_variant_subtag("1234"_sv));

    EXPECT(!Unicode::is_unicode_variant_subtag(""_sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("a"_sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("aa"_sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("aaa"_sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("aaaa"_sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("aaaaaaaaa"_sv));
    EXPECT(!Unicode::is_unicode_variant_subtag("a234"_sv));
}

TEST_CASE(is_type_identifier)
{
    EXPECT(Unicode::is_type_identifier("aaaa"_sv));
    EXPECT(Unicode::is_type_identifier("aaaa-bbbb"_sv));
    EXPECT(Unicode::is_type_identifier("aaaa-bbbb-cccc"_sv));

    EXPECT(Unicode::is_type_identifier("1aaa"_sv));
    EXPECT(Unicode::is_type_identifier("12aa"_sv));
    EXPECT(Unicode::is_type_identifier("123a"_sv));
    EXPECT(Unicode::is_type_identifier("1234"_sv));

    EXPECT(!Unicode::is_type_identifier(""_sv));
    EXPECT(!Unicode::is_type_identifier("a"_sv));
    EXPECT(!Unicode::is_type_identifier("aa"_sv));
    EXPECT(!Unicode::is_type_identifier("aaaaaaaaa"_sv));
    EXPECT(!Unicode::is_type_identifier("aaaa-"_sv));
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
    auto pass = [](StringView locale, Optional<StringView> expected_language, Optional<StringView> expected_script, Optional<StringView> expected_region, Vector<StringView> expected_variants) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        VERIFY(locale_id.has_value());

        EXPECT_EQ(locale_id->language_id.language, expected_language);
        EXPECT_EQ(locale_id->language_id.script, expected_script);
        EXPECT_EQ(locale_id->language_id.region, expected_region);
        EXPECT(compare_vectors(locale_id->language_id.variants, expected_variants));
    };

    fail("a"_sv);
    fail("1234"_sv);
    fail("aaa-"_sv);
    fail("aaa-cc-"_sv);
    fail("aaa-bbbb-cc-"_sv);
    fail("aaa-bbbb-cc-123"_sv);

    pass("aaa"_sv, "aaa"_sv, {}, {}, {});
    pass("aaa-bbbb"_sv, "aaa"_sv, "bbbb"_sv, {}, {});
    pass("aaa-cc"_sv, "aaa"_sv, {}, "cc"_sv, {});
    pass("aaa-bbbb-cc"_sv, "aaa"_sv, "bbbb"_sv, "cc"_sv, {});
    pass("aaa-bbbb-cc-1234"_sv, "aaa"_sv, "bbbb"_sv, "cc"_sv, { "1234"_sv });
    pass("aaa-bbbb-cc-1234-5678"_sv, "aaa"_sv, "bbbb"_sv, "cc"_sv, { "1234"_sv, "5678"_sv });
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

    fail("en-u"_sv);
    fail("en-u-"_sv);
    fail("en-u-x"_sv);
    fail("en-u-xx-"_sv);
    fail("en-u--xx"_sv);
    fail("en-u-xx-xxxxx-"_sv);
    fail("en-u-xx--xxxxx"_sv);
    fail("en-u-xx-xxxxxxxxx"_sv);
    fail("en-u-xxxxx-"_sv);
    fail("en-u-xxxxxxxxx"_sv);

    pass("en-u-xx"_sv, { {}, { { "xx"_sv, ""_sv } } });
    pass("en-u-xx-yyyy"_sv, { {}, { { "xx"_sv, { "yyyy"_sv } } } });
    pass("en-u-xx-yyyy-zzzz"_sv, { {}, { { "xx"_sv, "yyyy-zzzz"_sv } } });
    pass("en-u-xx-yyyy-zzzz-aa"_sv, { {}, { { "xx"_sv, "yyyy-zzzz"_sv }, { "aa"_sv, ""_sv } } });
    pass("en-u-xxx"_sv, { { "xxx"_sv }, {} });
    pass("en-u-fff-gggg"_sv, { { "fff"_sv, "gggg"_sv }, {} });
    pass("en-u-fff-xx"_sv, { { "fff"_sv }, { { "xx"_sv, ""_sv } } });
    pass("en-u-fff-xx-yyyy"_sv, { { "fff"_sv }, { { "xx"_sv, "yyyy"_sv } } });
    pass("en-u-fff-gggg-xx-yyyy"_sv, { { "fff"_sv, "gggg"_sv }, { { "xx"_sv, "yyyy"_sv } } });
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

    fail("en-t"_sv);
    fail("en-t-"_sv);
    fail("en-t-a"_sv);
    fail("en-t-en-"_sv);
    fail("en-t-root"_sv);
    fail("en-t-aaaaaaaaa"_sv);
    fail("en-t-en-aaa"_sv);
    fail("en-t-en-latn-latn"_sv);
    fail("en-t-en-a"_sv);
    fail("en-t-en-00"_sv);
    fail("en-t-en-latn-0"_sv);
    fail("en-t-en-latn-00"_sv);
    fail("en-t-en-latn-xyz"_sv);
    fail("en-t-en-aaaaaaaaa"_sv);
    fail("en-t-en-latn-gb-aaaa"_sv);
    fail("en-t-en-latn-gb-aaaaaaaaa"_sv);
    fail("en-t-k0"_sv);
    fail("en-t-k0-aa"_sv);
    fail("en-t-k0-aaaaaaaaa"_sv);

    pass("en-t-en"_sv, { TransformedExtension::LanguageID { false, "en"_sv }, {} });
    pass("en-t-en-latn"_sv, { TransformedExtension::LanguageID { false, "en"_sv, "latn"_sv }, {} });
    pass("en-t-en-us"_sv, { TransformedExtension::LanguageID { false, "en"_sv, {}, "us"_sv }, {} });
    pass("en-t-en-latn-us"_sv, { TransformedExtension::LanguageID { false, "en"_sv, "latn"_sv, "us"_sv }, {} });
    pass("en-t-en-posix"_sv, { TransformedExtension::LanguageID { false, "en"_sv, {}, {}, { "posix"_sv } }, {} });
    pass("en-t-en-latn-posix"_sv, { TransformedExtension::LanguageID { false, "en"_sv, "latn"_sv, {}, { "posix"_sv } }, {} });
    pass("en-t-en-us-posix"_sv, { TransformedExtension::LanguageID { false, "en"_sv, {}, "us"_sv, { "posix"_sv } }, {} });
    pass("en-t-en-latn-us-posix"_sv, { TransformedExtension::LanguageID { false, "en"_sv, "latn"_sv, "us"_sv, { "posix"_sv } }, {} });
    pass("en-t-k0-aaa"_sv, { {}, { { "k0"_sv, { "aaa"_sv } } } });
    pass("en-t-k0-aaa-bbbb"_sv, { {}, { { "k0"_sv, "aaa-bbbb"_sv } } });
    pass("en-t-k0-aaa-k1-bbbb"_sv, { {}, { { "k0"_sv, { "aaa"_sv } }, { "k1"_sv, "bbbb"_sv } } });
    pass("en-t-en-k0-aaa"_sv, { TransformedExtension::LanguageID { false, "en"_sv }, { { "k0"_sv, "aaa"_sv } } });
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

    fail("en-z"_sv);
    fail("en-0"_sv);
    fail("en-z-"_sv);
    fail("en-0-"_sv);
    fail("en-z-a"_sv);
    fail("en-0-a"_sv);
    fail("en-z-aaaaaaaaa"_sv);
    fail("en-0-aaaaaaaaa"_sv);
    fail("en-z-aaa-"_sv);
    fail("en-0-aaa-"_sv);
    fail("en-z-aaa-a"_sv);
    fail("en-0-aaa-a"_sv);

    pass("en-z-aa"_sv, { 'z', "aa"_sv });
    pass("en-z-aa-bbb"_sv, { 'z', "aa-bbb"_sv });
    pass("en-z-aa-bbb-cccccccc"_sv, { 'z', "aa-bbb-cccccccc"_sv });
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

    fail("en-x"_sv);
    fail("en-x-"_sv);
    fail("en-x-aaaaaaaaa"_sv);
    fail("en-x-aaa-"_sv);
    fail("en-x-aaa-aaaaaaaaa"_sv);

    pass("en-x-a"_sv, { "a"_sv });
    pass("en-x-aaaaaaaa"_sv, { "aaaaaaaa"_sv });
    pass("en-x-aaa-bbb"_sv, { "aaa"_sv, "bbb"_sv });
    pass("en-x-aaa-x-bbb"_sv, { "aaa"_sv, "x"_sv, "bbb"_sv });
}

TEST_CASE(canonicalize_unicode_locale_id)
{
    auto test = [](StringView locale, StringView expected_canonical_locale) {
        auto canonical_locale = Unicode::canonicalize_unicode_locale_id(locale);
        EXPECT_EQ(canonical_locale, expected_canonical_locale);
    };

    test("aaa"_sv, "aaa"_sv);
    test("AaA"_sv, "aaa"_sv);
    test("aaa-bbbb"_sv, "aaa-Bbbb"_sv);
    test("aaa-cc"_sv, "aaa-CC"_sv);
    test("aaa-bBBB-cC"_sv, "aaa-Bbbb-CC"_sv);
    test("aaa-bbbb-cc-1234"_sv, "aaa-Bbbb-CC-1234"_sv);
    test("aaa-bbbb-cc-ABCDE"_sv, "aaa-Bbbb-CC-abcde"_sv);

    test("en-u-aa"_sv, "en-u-aa"_sv);
    test("EN-U-AA"_sv, "en-u-aa"_sv);
    test("en-u-aa-bbb"_sv, "en-u-aa-bbb"_sv);
    test("EN-U-AA-BBB"_sv, "en-u-aa-bbb"_sv);
    test("en-u-aa-ccc-bbb"_sv, "en-u-aa-ccc-bbb"_sv);
    test("EN-U-AA-CCC-BBB"_sv, "en-u-aa-ccc-bbb"_sv);
    test("en-u-ddd-bbb-ccc"_sv, "en-u-bbb-ccc-ddd"_sv);
    test("EN-U-DDD-BBB-CCC"_sv, "en-u-bbb-ccc-ddd"_sv);
    test("en-u-2k-aaa-1k-bbb"_sv, "en-u-1k-bbb-2k-aaa"_sv);
    test("EN-U-2K-AAA-1K-BBB"_sv, "en-u-1k-bbb-2k-aaa"_sv);
    test("en-u-ccc-bbb-2k-aaa-1k-bbb"_sv, "en-u-bbb-ccc-1k-bbb-2k-aaa"_sv);
    test("EN-U-CCC-BBB-2K-AAA-1K-BBB"_sv, "en-u-bbb-ccc-1k-bbb-2k-aaa"_sv);
    test("en-u-1k-true"_sv, "en-u-1k"_sv);
    test("EN-U-1K-TRUE"_sv, "en-u-1k"_sv);
    test("en-u-1k-true-abcd"_sv, "en-u-1k-true-abcd"_sv);
    test("EN-U-1K-TRUE-ABCD"_sv, "en-u-1k-true-abcd"_sv);
    test("en-u-kb-yes"_sv, "en-u-kb"_sv);
    test("EN-U-KB-YES"_sv, "en-u-kb"_sv);
    test("en-u-kb-yes-abcd"_sv, "en-u-kb-yes-abcd"_sv);
    test("EN-U-KB-YES-ABCD"_sv, "en-u-kb-yes-abcd"_sv);
    test("en-u-ka-yes"_sv, "en-u-ka"_sv);
    test("EN-U-KA-YES"_sv, "en-u-ka"_sv);
    test("en-u-1k-names"_sv, "en-u-1k-names"_sv);
    test("EN-U-1K-NAMES"_sv, "en-u-1k-names"_sv);
    test("en-u-ks-primary"_sv, "en-u-ks-level1"_sv);
    test("EN-U-KS-PRIMARY"_sv, "en-u-ks-level1"_sv);
    test("en-u-ka-primary"_sv, "en-u-ka-primary"_sv);
    test("EN-U-KA-PRIMARY"_sv, "en-u-ka-primary"_sv);
    test("en-u-ms-imperial"_sv, "en-u-ms-uksystem"_sv);
    test("EN-U-MS-IMPERIAL"_sv, "en-u-ms-uksystem"_sv);
    test("en-u-ma-imperial"_sv, "en-u-ma-imperial"_sv);
    test("EN-U-MA-IMPERIAL"_sv, "en-u-ma-imperial"_sv);
    test("en-u-tz-hongkong"_sv, "en-u-tz-hkhkg"_sv);
    test("EN-U-TZ-HONGKONG"_sv, "en-u-tz-hkhkg"_sv);
    test("en-u-ta-hongkong"_sv, "en-u-ta-hongkong"_sv);
    test("EN-U-TA-HONGKONG"_sv, "en-u-ta-hongkong"_sv);
    test("en-u-ca-ethiopic-amete-alem"_sv, "en-u-ca-ethioaa"_sv);
    test("EN-U-CA-ETHIOPIC-AMETE-ALEM"_sv, "en-u-ca-ethioaa"_sv);
    test("en-u-ca-alem-ethiopic-amete"_sv, "en-u-ca-alem-ethiopic-amete"_sv);
    test("EN-U-CA-ALEM-ETHIOPIC-AMETE"_sv, "en-u-ca-alem-ethiopic-amete"_sv);
    test("en-u-ca-ethiopic-amete-xxx-alem"_sv, "en-u-ca-ethiopic-amete-xxx-alem"_sv);
    test("EN-U-CA-ETHIOPIC-AMETE-XXX-ALEM"_sv, "en-u-ca-ethiopic-amete-xxx-alem"_sv);
    test("en-u-cb-ethiopic-amete-alem"_sv, "en-u-cb-ethiopic-amete-alem"_sv);
    test("EN-U-CB-ETHIOPIC-AMETE-ALEM"_sv, "en-u-cb-ethiopic-amete-alem"_sv);

    test("en-t-en"_sv, "en-t-en"_sv);
    test("EN-T-EN"_sv, "en-t-en"_sv);
    test("en-latn-t-en-latn"_sv, "en-Latn-t-en-latn"_sv);
    test("EN-LATN-T-EN-LATN"_sv, "en-Latn-t-en-latn"_sv);
    test("en-us-t-en-us"_sv, "en-US-t-en-us"_sv);
    test("EN-US-T-EN-US"_sv, "en-US-t-en-us"_sv);
    test("en-latn-us-t-en-latn-us"_sv, "en-Latn-US-t-en-latn-us"_sv);
    test("EN-LATN-US-T-EN-LATN-US"_sv, "en-Latn-US-t-en-latn-us"_sv);
    test("en-t-en-k2-bbb-k1-aaa"_sv, "en-t-en-k1-aaa-k2-bbb"_sv);
    test("EN-T-EN-K2-BBB-K1-AAA"_sv, "en-t-en-k1-aaa-k2-bbb"_sv);
    test("en-t-k1-true"_sv, "en-t-k1-true"_sv);
    test("EN-T-K1-TRUE"_sv, "en-t-k1-true"_sv);
    test("en-t-k1-yes"_sv, "en-t-k1-yes"_sv);
    test("EN-T-K1-YES"_sv, "en-t-k1-yes"_sv);
    test("en-t-m0-names"_sv, "en-t-m0-prprname"_sv);
    test("EN-T-M0-NAMES"_sv, "en-t-m0-prprname"_sv);
    test("en-t-k1-names"_sv, "en-t-k1-names"_sv);
    test("EN-T-K1-NAMES"_sv, "en-t-k1-names"_sv);
    test("en-t-k1-primary"_sv, "en-t-k1-primary"_sv);
    test("EN-T-K1-PRIMARY"_sv, "en-t-k1-primary"_sv);
    test("en-t-k1-imperial"_sv, "en-t-k1-imperial"_sv);
    test("EN-T-K1-IMPERIAL"_sv, "en-t-k1-imperial"_sv);
    test("en-t-k1-hongkong"_sv, "en-t-k1-hongkong"_sv);
    test("EN-T-K1-HONGKONG"_sv, "en-t-k1-hongkong"_sv);
    test("en-t-k1-ethiopic-amete-alem"_sv, "en-t-k1-ethiopic-amete-alem"_sv);
    test("EN-T-K1-ETHIOPIC-AMETE-ALEM"_sv, "en-t-k1-ethiopic-amete-alem"_sv);

    test("en-0-aaa"_sv, "en-0-aaa"_sv);
    test("EN-0-AAA"_sv, "en-0-aaa"_sv);
    test("en-0-bbb-aaa"_sv, "en-0-bbb-aaa"_sv);
    test("EN-0-BBB-AAA"_sv, "en-0-bbb-aaa"_sv);
    test("en-z-bbb-0-aaa"_sv, "en-0-aaa-z-bbb"_sv);
    test("EN-Z-BBB-0-AAA"_sv, "en-0-aaa-z-bbb"_sv);

    test("en-x-aa"_sv, "en-x-aa"_sv);
    test("EN-X-AA"_sv, "en-x-aa"_sv);
    test("en-x-bbb-aa"_sv, "en-x-bbb-aa"_sv);
    test("EN-X-BBB-AA"_sv, "en-x-bbb-aa"_sv);

    test("en-u-aa-t-en"_sv, "en-t-en-u-aa"_sv);
    test("EN-U-AA-T-EN"_sv, "en-t-en-u-aa"_sv);
    test("en-z-bbb-u-aa-t-en-0-aaa"_sv, "en-0-aaa-t-en-u-aa-z-bbb"_sv);
    test("EN-Z-BBB-U-AA-T-EN-0-AAA"_sv, "en-0-aaa-t-en-u-aa-z-bbb"_sv);
    test("en-z-bbb-u-aa-t-en-0-aaa-x-ccc"_sv, "en-0-aaa-t-en-u-aa-z-bbb-x-ccc"_sv);
    test("EN-Z-BBB-U-AA-T-EN-0-AAA-X-CCC"_sv, "en-0-aaa-t-en-u-aa-z-bbb-x-ccc"_sv);

    // Language subtag aliases.
    test("sh"_sv, "sr-Latn"_sv);
    test("SH"_sv, "sr-Latn"_sv);
    test("sh-cyrl"_sv, "sr-Cyrl"_sv);
    test("SH-CYRL"_sv, "sr-Cyrl"_sv);
    test("cnr"_sv, "sr-ME"_sv);
    test("CNR"_sv, "sr-ME"_sv);
    test("cnr-ba"_sv, "sr-BA"_sv);
    test("CNR-BA"_sv, "sr-BA"_sv);

    // Territory subtag aliases.
    test("ru-su"_sv, "ru-RU"_sv);
    test("RU-SU"_sv, "ru-RU"_sv);
    test("ru-810"_sv, "ru-RU"_sv);
    test("RU-810"_sv, "ru-RU"_sv);
    test("en-su"_sv, "en-RU"_sv);
    test("EN-SU"_sv, "en-RU"_sv);
    test("en-810"_sv, "en-RU"_sv);
    test("EN-810"_sv, "en-RU"_sv);
    test("hy-su"_sv, "hy-AM"_sv);
    test("HY-SU"_sv, "hy-AM"_sv);
    test("hy-810"_sv, "hy-AM"_sv);
    test("HY-810"_sv, "hy-AM"_sv);
    test("und-Armn-su"_sv, "und-Armn-AM"_sv);
    test("UND-ARMN-SU"_sv, "und-Armn-AM"_sv);
    test("und-Armn-810"_sv, "und-Armn-AM"_sv);
    test("UND-ARMN-810"_sv, "und-Armn-AM"_sv);

    // Script subtag aliases.
    test("en-qaai"_sv, "en-Zinh"_sv);
    test("EN-QAAI"_sv, "en-Zinh"_sv);

    // Variant subtag aliases.
    test("en-polytoni"_sv, "en-polyton"_sv);
    test("EN-POLYTONI"_sv, "en-polyton"_sv);

    // Subdivision subtag aliases.
    test("en-u-sd-cn11"_sv, "en-u-sd-cnbj"_sv);
    test("EN-U-SD-CN11"_sv, "en-u-sd-cnbj"_sv);
    test("en-u-rg-cn12"_sv, "en-u-rg-cntj"_sv);
    test("EN-U-RG-CN12"_sv, "en-u-rg-cntj"_sv);
    test("en-u-aa-cn11"_sv, "en-u-aa-cn11"_sv);
    test("EN-U-AA-CN11"_sv, "en-u-aa-cn11"_sv);

    // Complex aliases.
    test("en-lojban"_sv, "en"_sv);
    test("EN-LOJBAN"_sv, "en"_sv);
    test("art-lojban"_sv, "jbo"_sv);
    test("ART-LOJBAN"_sv, "jbo"_sv);
    test("cel-gaulish"_sv, "xtg"_sv);
    test("CEL-GAULISH"_sv, "xtg"_sv);
    test("zh-guoyu"_sv, "zh"_sv);
    test("ZH-GUOYU"_sv, "zh"_sv);
    test("zh-hakka"_sv, "hak"_sv);
    test("ZH-HAKKA"_sv, "hak"_sv);
    test("zh-xiang"_sv, "hsn"_sv);
    test("ZH-XIANG"_sv, "hsn"_sv);
    test("ja-latn-hepburn-heploc"_sv, "ja-Latn-alalc97"_sv);
    test("JA-LATN-HEPBURN-HEPLOC"_sv, "ja-Latn-alalc97"_sv);

    // Default content.
    test("en-us"_sv, "en-US"_sv);
    test("EN-US"_sv, "en-US"_sv);
    test("zh-Hans-CN"_sv, "zh-Hans-CN"_sv);
    test("ZH-HANS-CN"_sv, "zh-Hans-CN"_sv);
}

TEST_CASE(supports_locale_aliases)
{
    EXPECT(Unicode::is_locale_available("zh"_sv));
    EXPECT(Unicode::is_locale_available("zh-Hant"_sv));
    EXPECT(Unicode::is_locale_available("zh-TW"_sv));
    EXPECT(Unicode::is_locale_available("zh-Hant-TW"_sv));
}
