/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/MappedFile.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/TextLayout.h>
#include <LibTest/TestCase.h>
#include <harfbuzz/hb.h>

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

static NonnullRefPtr<Gfx::Font> load_text_font(float point_size)
{
    auto file = MUST(Core::MappedFile::map(TEST_INPUT("fonts/text.ttf"sv)));
    // The typeface outlives the mapping, so let it own a copy of the font data.
    auto typeface = MUST(Gfx::Typeface::try_load_from_temporary_memory(file->bytes()));
    return adopt_ref(*new Gfx::Font(typeface, point_size, point_size, {}, {}));
}

static NonnullRefPtr<Gfx::GlyphRun> shape(Gfx::Font const& font, StringView text)
{
    auto utf16_text = Utf16String::from_utf8(text);
    return Gfx::shape_text({}, 0, 0, utf16_text.utf16_view(), font, Gfx::GlyphRun::TextType::Common);
}

// The run's bounding box is conservative: it contains every glyph's own extents at the glyph's origin, and it
// scales with the device scale.
TEST_CASE(glyph_run_bounding_box_contains_glyph_extents)
{
    auto font = load_text_font(16);
    auto run = shape(font, "abc"sv);
    EXPECT_EQ(run->glyphs().size(), 3u);

    auto bounds = Gfx::glyph_run_bounding_box(font, run->glyphs(), 1);
    EXPECT(!bounds.is_empty());

    auto* hb_font = font->harfbuzz_font();
    int x_scale = 0;
    int y_scale = 0;
    hb_font_get_scale(hb_font, &x_scale, &y_scale);
    auto font_ascent = font->pixel_metrics().ascent;
    for (auto const& glyph : run->glyphs()) {
        hb_glyph_extents_t extents {};
        EXPECT(hb_font_get_glyph_extents(hb_font, glyph.glyph_id, &extents));
        Gfx::FloatRect glyph_bounds {
            glyph.position.x() + extents.x_bearing * font->pixel_size() / x_scale,
            glyph.position.y() + font_ascent - extents.y_bearing * font->pixel_size() / y_scale,
            extents.width * font->pixel_size() / x_scale,
            -extents.height * font->pixel_size() / y_scale,
        };
        EXPECT(!glyph_bounds.is_empty());
        EXPECT(bounds.contains(glyph_bounds));
    }

    auto scaled_bounds = Gfx::glyph_run_bounding_box(font, run->glyphs(), 2);
    EXPECT_APPROXIMATE(scaled_bounds.x(), bounds.x() * 2);
    EXPECT_APPROXIMATE(scaled_bounds.y(), bounds.y() * 2);
    EXPECT_APPROXIMATE(scaled_bounds.width(), bounds.width() * 2);
    EXPECT_APPROXIMATE(scaled_bounds.height(), bounds.height() * 2);

    auto empty_run = shape(font, ""sv);
    EXPECT(Gfx::glyph_run_bounding_box(font, empty_run->glyphs(), 1).is_empty());
}

// Glyph intercepts report one [start, end] interval per glyph whose ink crosses the band, in run order and in
// run-local device pixels.
TEST_CASE(glyph_run_intercepts)
{
    auto font = load_text_font(16);
    auto run = shape(font, "abc"sv);

    // Bands are relative to the run's baseline origin; ink above the baseline has negative y.
    auto mid_x_height = -font->pixel_metrics().x_height / 2;
    auto intercepts = Gfx::glyph_run_glyph_intercepts(font, run->glyphs(), 1, mid_x_height - 1, mid_x_height + 1);
    EXPECT_EQ(intercepts.size(), 6u);
    float previous_end = 0;
    for (size_t i = 0; i + 1 < intercepts.size(); i += 2) {
        EXPECT(intercepts[i] < intercepts[i + 1]);
        EXPECT(intercepts[i] >= previous_end);
        EXPECT(intercepts[i + 1] <= run->width() + 1);
        previous_end = intercepts[i + 1];
    }

    // At twice the scale the intervals scale with it.
    auto scaled_intercepts = Gfx::glyph_run_glyph_intercepts(font, run->glyphs(), 2, (mid_x_height - 1) * 2, (mid_x_height + 1) * 2);
    EXPECT_EQ(scaled_intercepts.size(), 6u);
    for (size_t i = 0; i < intercepts.size(); ++i)
        EXPECT(AK::fabs(scaled_intercepts[i] - intercepts[i] * 2) < 0.01f);

    // A band far above the ascender touches no ink.
    EXPECT(Gfx::glyph_run_glyph_intercepts(font, run->glyphs(), 1, -200, -190).is_empty());

    // A band just below the top of the 'b' ascender is only reached by that glyph.
    auto* hb_font = font->harfbuzz_font();
    int x_scale = 0;
    int y_scale = 0;
    hb_font_get_scale(hb_font, &x_scale, &y_scale);
    hb_glyph_extents_t b_extents {};
    EXPECT(hb_font_get_glyph_extents(hb_font, run->glyphs()[1].glyph_id, &b_extents));
    auto b_top = -b_extents.y_bearing * font->pixel_size() / y_scale;
    auto ascender_intercepts = Gfx::glyph_run_glyph_intercepts(font, run->glyphs(), 1, b_top + 0.5f, b_top + 1.5f);
    EXPECT_EQ(ascender_intercepts.size(), 2u);
    EXPECT(ascender_intercepts[0] >= run->glyphs()[1].position.x());
    EXPECT(ascender_intercepts[1] <= run->glyphs()[2].position.x());

    // A degenerate scale produces nothing.
    EXPECT(Gfx::glyph_run_glyph_intercepts(font, run->glyphs(), 0, mid_x_height - 1, mid_x_height + 1).is_empty());
}
