/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibLocale/DisplayNames.h>
#include <LibLocale/Locale.h>

TEST_CASE(locale_mappings_en)
{
    auto language = Locale::language_display_name("en"sv, "en"sv, Locale::LanguageDisplay::Standard);
    EXPECT(language.has_value());
    EXPECT_EQ(*language, "English"sv);

    language = Locale::language_display_name("en"sv, "i-defintely-don't-exist"sv, Locale::LanguageDisplay::Standard);
    EXPECT(!language.has_value());

    auto territory = Locale::region_display_name("en"sv, "US"sv);
    EXPECT(territory.has_value());
    EXPECT_EQ(*territory, "United States"sv);

    territory = Locale::region_display_name("en"sv, "i-defintely-don't-exist"sv);
    EXPECT(!territory.has_value());

    auto script = Locale::script_display_name("en"sv, "Latn"sv);
    EXPECT(script.has_value());
    EXPECT_EQ(*script, "Latin"sv);

    script = Locale::script_display_name("en"sv, "i-defintely-don't-exist"sv);
    EXPECT(!script.has_value());
}

TEST_CASE(locale_mappings_fr)
{
    auto language = Locale::language_display_name("fr"sv, "en"sv, Locale::LanguageDisplay::Standard);
    EXPECT(language.has_value());
    EXPECT_EQ(*language, "anglais"sv);

    language = Locale::language_display_name("fr"sv, "i-defintely-don't-exist"sv, Locale::LanguageDisplay::Standard);
    EXPECT(!language.has_value());

    auto territory = Locale::region_display_name("fr"sv, "US"sv);
    EXPECT(territory.has_value());
    EXPECT_EQ(*territory, "États-Unis"sv);

    territory = Locale::region_display_name("fr"sv, "i-defintely-don't-exist"sv);
    EXPECT(!territory.has_value());

    auto script = Locale::script_display_name("fr"sv, "Latn"sv);
    EXPECT(script.has_value());
    EXPECT_EQ(*script, "latin"sv);

    script = Locale::script_display_name("fr"sv, "i-defintely-don't-exist"sv);
    EXPECT(!script.has_value());
}

TEST_CASE(locale_mappings_root)
{
    auto language = Locale::language_display_name("und"sv, "en"sv, Locale::LanguageDisplay::Standard);
    EXPECT(language.has_value());
    EXPECT_EQ(*language, "en"sv);

    language = Locale::language_display_name("und"sv, "i-defintely-don't-exist"sv, Locale::LanguageDisplay::Standard);
    EXPECT(!language.has_value());

    auto territory = Locale::region_display_name("und"sv, "US"sv);
    EXPECT(territory.has_value());
    EXPECT_EQ(*territory, "US"sv);

    territory = Locale::region_display_name("und"sv, "i-defintely-don't-exist"sv);
    EXPECT(!territory.has_value());

    auto script = Locale::script_display_name("und"sv, "Latn"sv);
    EXPECT(script.has_value());
    EXPECT_EQ(*script, "Latn"sv);

    script = Locale::script_display_name("und"sv, "i-defintely-don't-exist"sv);
    EXPECT(!script.has_value());
}
