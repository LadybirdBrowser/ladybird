/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/SystemFontMatcher.h>
#include <LibTest/TestCase.h>

#include <fontconfig/fontconfig.h>

TEST_SETUP
{
    auto path = MUST(FileSystem::real_path("test-inputs/fonts/text.ttf"sv));
    auto* config = FcConfigCreate();
    static ScopeGuard cleanup = [config] {
        FcConfigAppFontClear(config);
        FcConfigDestroy(config);
    };
    VERIFY(FcConfigAppFontAddFile(config, reinterpret_cast<FcChar8 const*>(path.characters())));
    auto* fonts = FcConfigGetFonts(config, FcSetApplication);
    VERIFY(fonts && fonts->nfont == 1);

    // The first candidate is no longer available. The next one has exactly the same character coverage.
    auto* missing = FcPatternDuplicate(fonts->fonts[0]);
    FcPatternDel(missing, FC_FILE);
    FcPatternAddString(missing, FC_FILE, reinterpret_cast<FcChar8 const*>("missing-font.ttf"));
    VERIFY(FcFontSetAdd(fonts, missing));
    swap(fonts->fonts[0], fonts->fonts[1]);
    VERIFY(FcConfigAppFontAddFile(config, reinterpret_cast<FcChar8 const*>("test-inputs/fonts/styles.ttc")));

    // Lato covers the requested character on disk, but the configuration excludes its glyphs for bold text.
    constexpr auto rules = R"(<fontconfig>
        <match target="pattern">
            <edit name="family" mode="prepend" binding="strong"><string>Lato</string></edit>
        </match>
        <match target="font">
            <test name="family"><string>Lato</string></test>
            <test target="pattern" name="weight"><const>bold</const></test>
            <edit name="charset" mode="assign"><charset/></edit>
        </match>
    </fontconfig>)";
    VERIFY(FcConfigParseAndLoadFromMemory(config, reinterpret_cast<FcChar8 const*>(rules), FcTrue));
    VERIFY(FcConfigSetCurrent(config));
}

TEST_CASE(fallback_skips_an_unloadable_face_with_identical_coverage)
{
    auto path = MUST(FileSystem::real_path("test-inputs/fonts/text.ttf"sv));
    auto match = Gfx::SystemFontMatcher::match_code_point('A', 400, Gfx::FontWidth::Normal, 0, false);
    EXPECT(match.has_value());
    if (match.has_value())
        EXPECT_EQ(match->get<Gfx::SystemFontFile>().path.bytes_as_string_view(), path.view());
}

TEST_CASE(fallback_respects_configured_character_coverage)
{
    auto path = MUST(FileSystem::real_path("test-inputs/fonts/styles.ttc"sv));
    auto match = Gfx::SystemFontMatcher::match_code_point('B', 700, Gfx::FontWidth::Normal, 0, false);
    EXPECT(match.has_value());
    if (match.has_value())
        EXPECT_EQ(match->get<Gfx::SystemFontFile>().path.bytes_as_string_view(), path.view());
}
