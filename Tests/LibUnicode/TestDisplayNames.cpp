/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibUnicode/DisplayNames.h>
#include <LibUnicode/Locale.h>

TEST_CASE(locale_mappings_en)
{
    auto language = Unicode::language_display_name("en"sv, "en"sv, Unicode::LanguageDisplay::Standard);
    EXPECT(language.has_value());
    EXPECT_EQ(*language, u"English"sv);

    language = Unicode::language_display_name("en"sv, "i-definitely-don't-exist"sv, Unicode::LanguageDisplay::Standard);
    EXPECT(!language.has_value());

    auto territory = Unicode::region_display_name("en"sv, "US"sv);
    EXPECT(territory.has_value());
    EXPECT_EQ(*territory, u"United States"sv);

    territory = Unicode::region_display_name("en"sv, "i-definitely-don't-exist"sv);
    EXPECT(!territory.has_value());

    auto script = Unicode::script_display_name("en"sv, "Latn"sv);
    EXPECT(script.has_value());
    EXPECT_EQ(*script, u"Latin"sv);

    script = Unicode::script_display_name("en"sv, "i-definitely-don't-exist"sv);
    EXPECT(!script.has_value());
}

TEST_CASE(locale_mappings_fr)
{
    auto language = Unicode::language_display_name("fr"sv, "en"sv, Unicode::LanguageDisplay::Standard);
    EXPECT(language.has_value());
    EXPECT_EQ(*language, u"anglais"sv);

    language = Unicode::language_display_name("fr"sv, "i-definitely-don't-exist"sv, Unicode::LanguageDisplay::Standard);
    EXPECT(!language.has_value());

    auto territory = Unicode::region_display_name("fr"sv, "US"sv);
    EXPECT(territory.has_value());
    EXPECT_EQ(*territory, u"États-Unis"sv);

    territory = Unicode::region_display_name("fr"sv, "i-definitely-don't-exist"sv);
    EXPECT(!territory.has_value());

    auto script = Unicode::script_display_name("fr"sv, "Latn"sv);
    EXPECT(script.has_value());
    EXPECT_EQ(*script, u"latin"sv);

    script = Unicode::script_display_name("fr"sv, "i-definitely-don't-exist"sv);
    EXPECT(!script.has_value());
}

TEST_CASE(locale_mappings_root)
{
    auto language = Unicode::language_display_name("und"sv, "en"sv, Unicode::LanguageDisplay::Standard);
    EXPECT(language.has_value());
    EXPECT_EQ(*language, u"en"sv);

    language = Unicode::language_display_name("und"sv, "i-definitely-don't-exist"sv, Unicode::LanguageDisplay::Standard);
    EXPECT(!language.has_value());

    auto territory = Unicode::region_display_name("und"sv, "US"sv);
    EXPECT(territory.has_value());
    EXPECT_EQ(*territory, u"US"sv);

    territory = Unicode::region_display_name("und"sv, "i-definitely-don't-exist"sv);
    EXPECT(!territory.has_value());

    auto script = Unicode::script_display_name("und"sv, "Latn"sv);
    EXPECT(script.has_value());
    EXPECT_EQ(*script, u"Latn"sv);

    script = Unicode::script_display_name("und"sv, "i-definitely-don't-exist"sv);
    EXPECT(!script.has_value());
}
