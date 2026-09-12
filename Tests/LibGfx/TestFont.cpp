/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/TextLayout.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <core/SkTypeface.h>
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

TEST_CASE(skia_typeface_retains_font_data_after_ladybird_typeface_is_destroyed)
{
    sk_sp<SkTypeface const> skia_typeface;
    ByteBuffer expected_table;
    constexpr auto cmap_tag = SkSetFourByteTag('c', 'm', 'a', 'p');
    {
        auto file = MUST(Core::MappedFile::map(TEST_INPUT("fonts/text.ttf"sv)));
        auto typeface = MUST(Gfx::Typeface::try_load_from_temporary_memory(file->bytes()));
        skia_typeface = sk_ref_sp(static_cast<Gfx::TypefaceSkia const&>(*typeface).sk_typeface());
        expected_table = MUST(ByteBuffer::create_uninitialized(skia_typeface->getTableSize(cmap_tag)));
        EXPECT(!expected_table.is_empty());
        EXPECT_EQ(skia_typeface->getTableData(cmap_tag, 0, expected_table.size(), expected_table.data()), expected_table.size());
    }
    auto actual_table = MUST(ByteBuffer::create_uninitialized(expected_table.size()));
    EXPECT_EQ(skia_typeface->getTableData(cmap_tag, 0, actual_table.size(), actual_table.data()), actual_table.size());
    EXPECT_EQ(actual_table, expected_table);
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

TEST_CASE(invisible_fallback_preserves_metrics_and_shaping)
{
    auto font = load_text_font(16);
    auto invisible = font->invisible_variant();
    EXPECT(invisible->is_invisible());
    EXPECT(!font->is_invisible());
    EXPECT_EQ(invisible->pixel_metrics().ascent, font->pixel_metrics().ascent);
    EXPECT_EQ(invisible->pixel_metrics().descent, font->pixel_metrics().descent);
    EXPECT_EQ(shape(*invisible, "abc"sv)->width(), shape(*font, "abc"sv)->width());
}

TEST_CASE(pending_font_only_hides_its_unicode_range)
{
    auto font = load_text_font(16);
    auto cascade = Gfx::FontCascadeList::create();
    cascade->add_pending_face({ { 'a', 'a' } }, [] { return Gfx::PendingFontState::Invisible; });
    cascade->add(font);
    cascade->set_last_resort_font(font);
    EXPECT(cascade->font_for_code_point('a').is_invisible());
    EXPECT(!cascade->font_for_code_point('b').is_invisible());
    EXPECT_EQ(&cascade->font_for_code_point('a'), &cascade->font_for_code_point('a'));
    EXPECT_EQ(cascade->first_available_font().pixel_size(), font->pixel_size());
}

TEST_CASE(pending_font_does_not_load_fallback_fonts)
{
    auto font = load_text_font(16);
    auto cascade = Gfx::FontCascadeList::create();
    u32 fallback_loads = 0;
    cascade->add_pending_face({ { 0, 0x10FFFF } }, [] { return Gfx::PendingFontState::Visible; });
    cascade->add_pending_face({ { 0, 0x10FFFF } }, [&] {
        ++fallback_loads;
        return Gfx::PendingFontState::Invisible;
    });
    cascade->add(font);
    cascade->set_last_resort_font(font);
    EXPECT_EQ(&cascade->font_for_code_point('a'), font.ptr());
    EXPECT_EQ(fallback_loads, 0u);
}

TEST_CASE(pending_font_can_resolve_synchronously_without_loading_fallbacks)
{
    auto local_font = load_text_font(24);
    auto fallback_font = load_text_font(16);
    auto cascade = Gfx::FontCascadeList::create();
    u32 local_loads = 0;
    u32 fallback_loads = 0;
    cascade->add_pending_face({ { 'a', 'a' } }, [&] {
        ++local_loads;
        return Gfx::PendingFontState::Invisible; }, [local_font] { return local_font; });
    cascade->add_pending_face({ { 'a', 'a' } }, [&] {
        ++fallback_loads;
        return Gfx::PendingFontState::Invisible;
    });
    cascade->add(fallback_font);
    cascade->set_last_resort_font(fallback_font);
    EXPECT_EQ(&cascade->font_for_code_point('b'), fallback_font.ptr());
    EXPECT_EQ(local_loads, 0u);
    EXPECT_EQ(&cascade->font_for_code_point('a'), local_font.ptr());
    EXPECT_EQ(&cascade->font_for_code_point('a'), local_font.ptr());
    EXPECT_EQ(local_loads, 1u);
    EXPECT_EQ(fallback_loads, 0u);
}

TEST_CASE(first_available_font_resolves_resident_faces_in_cascade_order)
{
    auto local_font = load_text_font(24);
    auto fallback_font = load_text_font(16);
    auto cascade = Gfx::FontCascadeList::create();
    bool resident = false;
    u32 excluded_loads = 0;
    u32 fallback_loads = 0;
    cascade->add_pending_face({ { 'a', 'a' } }, [&] {
        ++excluded_loads;
        return Gfx::PendingFontState::Invisible;
    });
    cascade->add_pending_face({ { ' ', ' ' } }, [] { return Gfx::PendingFontState::Invisible; }, [&]() -> RefPtr<Gfx::Font const> { return resident ? local_font.ptr() : nullptr; });
    cascade->add_pending_face({ { 0, 0x10FFFF } }, [&] {
        ++fallback_loads;
        return Gfx::PendingFontState::Visible;
    });
    cascade->add(fallback_font);
    cascade->set_last_resort_font(fallback_font);
    EXPECT_EQ(&cascade->first_available_font(), fallback_font.ptr());
    resident = true;
    EXPECT_EQ(&cascade->first_available_font(), local_font.ptr());
    EXPECT_EQ(excluded_loads, 0u);
    EXPECT_EQ(fallback_loads, 0u);

    auto preferred = Gfx::FontCascadeList::create();
    preferred->add(fallback_font);
    preferred->extend(*cascade);
    EXPECT_EQ(&preferred->first_available_font(), fallback_font.ptr());
}

TEST_CASE(loaded_font_precedes_pending_font_after_extending_cascade)
{
    auto font = load_text_font(16);
    auto cascade = Gfx::FontCascadeList::create();
    cascade->add(font);
    auto later_family = Gfx::FontCascadeList::create();
    u32 loads = 0;
    later_family->add_pending_face({ { 0, 0x10FFFF } }, [&] {
        ++loads;
        return Gfx::PendingFontState::Invisible;
    });
    cascade->extend(*later_family);
    cascade->set_last_resort_font(font);
    EXPECT_EQ(&cascade->font_for_code_point('a'), font.ptr());
    EXPECT_EQ(loads, 0u);
}

TEST_CASE(failed_font_allows_loading_the_next_face)
{
    auto font = load_text_font(16);
    auto cascade = Gfx::FontCascadeList::create();
    cascade->add_pending_face({ { 0, 0x10FFFF } }, [] { return Gfx::PendingFontState::Failed; });
    cascade->add_pending_face({ { 0, 0x10FFFF } }, [] { return Gfx::PendingFontState::Invisible; });
    cascade->set_last_resort_font(font);
    EXPECT(cascade->font_for_code_point('a').is_invisible());
}

#ifdef AK_OS_MACOS
static NonnullRefPtr<Gfx::Typeface const> round_trip_typeface_through_ipc(Gfx::Typeface const& typeface)
{
    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(typeface));

    auto data = message_buffer.take_data();
    FixedMemoryStream stream { data.span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return MUST(IPC::decode<NonnullRefPtr<Gfx::Typeface const>>(decoder));
}

TEST_CASE(system_ui_italic_keeps_its_slope_when_the_variation_clone_collapses)
{
    // At 28px the system font's opsz axis sits at its maximum, so applying the default variation hands back the base
    // CoreText UI font, which Skia reads as upright.
    float const font_size = 28;
    auto typeface = MUST(Gfx::TypefaceSkia::match_system_ui(Gfx::SystemUIFontKind::System, font_size, 400, Gfx::FontWidth::Normal, 1));
    EXPECT(typeface);

    Gfx::FontVariationSettings variations;
    variations.set_weight(400);
    variations.set_width(100);
    variations.set_optical_sizing(font_size);
    auto font = typeface->font(font_size * 0.75f, variations);
    EXPECT_EQ(font->typeface().slope(), 1u);

    auto decoded = round_trip_typeface_through_ipc(font->typeface());
    EXPECT_EQ(decoded->slope(), 1u);
    EXPECT_EQ(decoded->glyph_id_for_code_point('m'), font->typeface().glyph_id_for_code_point('m'));
}
#endif

TEST_CASE(shaping_cache_preserves_positions_spacing_and_trailing_whitespace)
{
    auto font = load_text_font(16);
    Gfx::TrailingWhitespace trailing_whitespace;
    auto origin = Gfx::shape_text({}, 2, 3, u"abc "sv, font, Gfx::GlyphRun::TextType::Common, &trailing_whitespace);
    auto translated = Gfx::shape_text({ 7, 11 }, 2, 3, u"abc "sv, font, Gfx::GlyphRun::TextType::Common);
    Gfx::TrailingWhitespace cached_trailing_whitespace;
    auto repeated = Gfx::shape_text({}, 2, 3, u"abc "sv, font, Gfx::GlyphRun::TextType::Common, &cached_trailing_whitespace);

    EXPECT_EQ(origin->width(), translated->width());
    EXPECT_EQ(origin->width(), repeated->width());
    EXPECT_EQ(origin->glyphs().size(), translated->glyphs().size());
    EXPECT_EQ(origin->glyphs().size(), repeated->glyphs().size());
    for (size_t i = 0; i < origin->glyphs().size(); ++i) {
        EXPECT_EQ(translated->glyphs()[i].position, origin->glyphs()[i].position.translated(7, 11));
        EXPECT_EQ(repeated->glyphs()[i].position, origin->glyphs()[i].position);
        EXPECT_EQ(repeated->glyphs()[i].glyph_id, origin->glyphs()[i].glyph_id);
    }
    EXPECT_EQ(trailing_whitespace.length_in_code_units, 1u);
    EXPECT_EQ(cached_trailing_whitespace.length_in_code_units, trailing_whitespace.length_in_code_units);
    EXPECT_EQ(cached_trailing_whitespace.advance, trailing_whitespace.advance);

    auto without_spacing = Gfx::shape_text({}, 0, 0, u"abc "sv, font, Gfx::GlyphRun::TextType::Common);
    EXPECT_APPROXIMATE(origin->width() - without_spacing->width(), 11.f);
}
