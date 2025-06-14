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
    auto language = Unicode::language_display_name("en"_sv, "en"_sv, Unicode::LanguageDisplay::Standard);
    EXPECT(language.has_value());
    EXPECT_EQ(*language, "English"_sv);

    language = Unicode::language_display_name("en"_sv, "i-definitely-don't-exist"_sv, Unicode::LanguageDisplay::Standard);
    EXPECT(!language.has_value());

    auto territory = Unicode::region_display_name("en"_sv, "US"_sv);
    EXPECT(territory.has_value());
    EXPECT_EQ(*territory, "United States"_sv);

    territory = Unicode::region_display_name("en"_sv, "i-definitely-don't-exist"_sv);
    EXPECT(!territory.has_value());

    auto script = Unicode::script_display_name("en"_sv, "Latn"_sv);
    EXPECT(script.has_value());
    EXPECT_EQ(*script, "Latin"_sv);

    script = Unicode::script_display_name("en"_sv, "i-definitely-don't-exist"_sv);
    EXPECT(!script.has_value());
}

TEST_CASE(locale_mappings_fr)
{
    auto language = Unicode::language_display_name("fr"_sv, "en"_sv, Unicode::LanguageDisplay::Standard);
    EXPECT(language.has_value());
    EXPECT_EQ(*language, "anglais"_sv);

    language = Unicode::language_display_name("fr"_sv, "i-definitely-don't-exist"_sv, Unicode::LanguageDisplay::Standard);
    EXPECT(!language.has_value());

    auto territory = Unicode::region_display_name("fr"_sv, "US"_sv);
    EXPECT(territory.has_value());
    EXPECT_EQ(*territory, "États-Unis"_sv);

    territory = Unicode::region_display_name("fr"_sv, "i-definitely-don't-exist"_sv);
    EXPECT(!territory.has_value());

    auto script = Unicode::script_display_name("fr"_sv, "Latn"_sv);
    EXPECT(script.has_value());
    EXPECT_EQ(*script, "latin"_sv);

    script = Unicode::script_display_name("fr"_sv, "i-definitely-don't-exist"_sv);
    EXPECT(!script.has_value());
}

TEST_CASE(locale_mappings_root)
{
    auto language = Unicode::language_display_name("und"_sv, "en"_sv, Unicode::LanguageDisplay::Standard);
    EXPECT(language.has_value());
    EXPECT_EQ(*language, "en"_sv);

    language = Unicode::language_display_name("und"_sv, "i-definitely-don't-exist"_sv, Unicode::LanguageDisplay::Standard);
    EXPECT(!language.has_value());

    auto territory = Unicode::region_display_name("und"_sv, "US"_sv);
    EXPECT(territory.has_value());
    EXPECT_EQ(*territory, "US"_sv);

    territory = Unicode::region_display_name("und"_sv, "i-definitely-don't-exist"_sv);
    EXPECT(!territory.has_value());

    auto script = Unicode::script_display_name("und"_sv, "Latn"_sv);
    EXPECT(script.has_value());
    EXPECT_EQ(*script, "Latn"_sv);

    script = Unicode::script_display_name("und"_sv, "i-definitely-don't-exist"_sv);
    EXPECT(!script.has_value());
}
