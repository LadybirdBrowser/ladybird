/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/MappedFile.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/Font/Typeface.h>
#include <LibTest/TestCase.h>

#define TEST_INPUT(x) ("test-inputs/" x)

namespace {

struct Global {
    Global()
    {
        Gfx::FontDatabase::the().install_system_font_provider(make<Gfx::PathFontProvider>());
    }
} global;

}

static bool font_is_emoji(StringView path)
{
    auto file = MUST(Core::MappedFile::map(path));
    auto typeface = MUST(Gfx::Typeface::try_load_from_externally_owned_memory(file->bytes()));
    // Construct the Font directly rather than via Typeface::font() — which would cache it on the
    // Typeface and form a Typeface<->Font reference cycle that leaks once both leave this scope.
    auto font = adopt_ref(*new Gfx::Font(typeface, 12, 12, {}, {}));
    return font->is_emoji_font();
}

// A COLRv1 color font is recognized.
TEST_CASE(colr_v1_font_is_emoji_font)
{
    EXPECT(font_is_emoji(TEST_INPUT("fonts/colrv1-noname.ttf"sv)));
}

// A monochrome emoji font is a text font, not a color emoji font.
TEST_CASE(monochrome_emoji_font_is_not_emoji_font)
{
    EXPECT(!font_is_emoji(TEST_INPUT("fonts/mono-emoji.ttf"sv)));
}

// A regular text font is not an emoji font.
TEST_CASE(text_font_is_not_emoji_font)
{
    EXPECT(!font_is_emoji(TEST_INPUT("fonts/text.ttf"sv)));
}
