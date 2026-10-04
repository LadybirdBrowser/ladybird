/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ConditionVariable.h>
#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/Font/SystemFallbackFonts.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/TextLayout.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <core/SkFont.h>
#include <core/SkStream.h>
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

class ConcurrentStart {
public:
    void wait()
    {
        MutexLocker locker(m_mutex);
        ++m_waiting;
        if (m_waiting == 8)
            m_condition.broadcast();
        else
            m_condition.wait_while([&] { return m_waiting < 8; });
    }

private:
    Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    size_t m_waiting { 0 };
};

static bool font_is_emoji(StringView path)
{
    auto typeface = MUST(Gfx::Typeface::try_load_from_mapped_file(MUST(Core::MappedFile::map(path)), 0));
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

static void check_skia_font_data_lifetime(Function<NonnullRefPtr<Gfx::Typeface>()> load_typeface)
{
    sk_sp<SkTypeface const> skia_typeface;
    ByteBuffer expected_table;
    ByteBuffer expected_font_data;
    constexpr auto cmap_tag = SkSetFourByteTag('c', 'm', 'a', 'p');
    {
        auto typeface = load_typeface();
        skia_typeface = sk_ref_sp(static_cast<Gfx::TypefaceSkia const&>(*typeface).sk_typeface());
        int collection_index = 0;
        auto stream = skia_typeface->openStream(&collection_index);
        EXPECT(stream);
        if (stream) {
            EXPECT_EQ(stream->getMemoryBase(), typeface->font_data().data());
            EXPECT_EQ(stream->getLength(), typeface->font_data().size());
        }
        expected_font_data = MUST(ByteBuffer::copy(typeface->font_data()));
        expected_table = MUST(ByteBuffer::create_uninitialized(skia_typeface->getTableSize(cmap_tag)));
        EXPECT(!expected_table.is_empty());
        EXPECT_EQ(skia_typeface->getTableData(cmap_tag, 0, expected_table.size(), expected_table.data()), expected_table.size());
    }
    auto actual_table = MUST(ByteBuffer::create_uninitialized(expected_table.size()));
    EXPECT_EQ(skia_typeface->getTableData(cmap_tag, 0, actual_table.size(), actual_table.data()), actual_table.size());
    EXPECT_EQ(actual_table, expected_table);
    int collection_index = 0;
    auto stream = skia_typeface->openStream(&collection_index);
    EXPECT(stream);
    if (stream) {
        auto actual_font_data = MUST(ByteBuffer::create_uninitialized(expected_font_data.size()));
        EXPECT_EQ(stream->read(actual_font_data.data(), actual_font_data.size()), actual_font_data.size());
        EXPECT_EQ(actual_font_data, expected_font_data);
    }
}

TEST_CASE(skia_typeface_retains_font_data_after_ladybird_typeface_is_destroyed)
{
    check_skia_font_data_lifetime([] {
        auto file = MUST(Core::MappedFile::map(TEST_INPUT("fonts/text.ttf"sv)));
        return MUST(Gfx::Typeface::try_load_from_temporary_memory(file->bytes()));
    });
}

TEST_CASE(skia_typeface_retains_mapped_font_data_after_ladybird_typeface_is_destroyed)
{
    check_skia_font_data_lifetime([] {
        auto file = MUST(Core::MappedFile::map(TEST_INPUT("fonts/text.ttf"sv)));
        return MUST(Gfx::Typeface::try_load_from_mapped_file(move(file)));
    });
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

TEST_CASE(pending_font_becoming_resident_replaces_ascii_fallback)
{
    auto local_font = load_text_font(24);
    auto fallback_font = load_text_font(16);
    auto cascade = Gfx::FontCascadeList::create();
    bool resident = false;
    cascade->add_pending_face({ { 'a', 'a' } }, [] { return Gfx::PendingFontState::Visible; }, [&]() -> RefPtr<Gfx::Font const> { return resident ? local_font.ptr() : nullptr; });
    cascade->add(fallback_font);
    cascade->set_last_resort_font(fallback_font);
    EXPECT_EQ(&cascade->font_for_code_point('a'), fallback_font.ptr());
    resident = true;
    EXPECT_EQ(&cascade->font_for_code_point('a'), local_font.ptr());
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

// Only ThreadSanitizer can catch a memo race here, since every thread reaches the same verdict.
TEST_CASE(emoji_classification_can_run_on_several_threads)
{
    auto typeface = MUST(Gfx::Typeface::try_load_from_mapped_file(MUST(Core::MappedFile::map(TEST_INPUT("fonts/colrv1-noname.ttf"sv))), 0));
    IGNORE_USE_IN_ESCAPING_LAMBDA auto font = adopt_ref(*new Gfx::Font(typeface, 12, 12, {}, {}));

    IGNORE_USE_IN_ESCAPING_LAMBDA Array<bool, 8> verdicts {};
    IGNORE_USE_IN_ESCAPING_LAMBDA ConcurrentStart start;
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t thread_index = 0; thread_index < verdicts.size(); ++thread_index) {
        auto thread = Threading::Thread::construct("EmojiVerdict"sv, [&font, &verdicts, &start, thread_index]() {
            start.wait();
            verdicts[thread_index] = font->is_emoji_font();
            return 0;
        });
        thread->start();
        threads.append(move(thread));
    }
    for (auto& thread : threads)
        (void)thread->join();

    for (auto verdict : verdicts)
        EXPECT(verdict);
}

// Only ThreadSanitizer can catch a memo race here, since every thread reads the same head table.
TEST_CASE(typeface_bounding_box_can_be_read_on_several_threads)
{
    auto file = MUST(Core::MappedFile::map(TEST_INPUT("fonts/text.ttf"sv)));
    IGNORE_USE_IN_ESCAPING_LAMBDA NonnullRefPtr<Gfx::Typeface const> typeface = MUST(Gfx::Typeface::try_load_from_temporary_memory(file->bytes()));

    IGNORE_USE_IN_ESCAPING_LAMBDA Array<Gfx::Typeface::BoundingBoxInFontUnits, 8> bounding_boxes {};
    IGNORE_USE_IN_ESCAPING_LAMBDA ConcurrentStart start;
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t thread_index = 0; thread_index < bounding_boxes.size(); ++thread_index) {
        auto thread = Threading::Thread::construct("TypefaceBoundingBox"sv, [typeface, &bounding_boxes, &start, thread_index]() {
            start.wait();
            bounding_boxes[thread_index] = typeface->bounding_box_in_font_units();
            return 0;
        });
        thread->start();
        threads.append(move(thread));
    }
    for (auto& thread : threads)
        (void)thread->join();

    auto expected = typeface->bounding_box_in_font_units();
    EXPECT(!expected.is_empty());

    for (auto const& bounding_box : bounding_boxes) {
        EXPECT_EQ(bounding_box.x_min, expected.x_min);
        EXPECT_EQ(bounding_box.y_min, expected.y_min);
        EXPECT_EQ(bounding_box.x_max, expected.x_max);
        EXPECT_EQ(bounding_box.y_max, expected.y_max);
        EXPECT_EQ(bounding_box.units_per_em, expected.units_per_em);
    }
}

// Only ThreadSanitizer can catch a memo race here, since fontconfig answers each scale the same way.
TEST_CASE(hinting_options_can_be_memoized_on_several_threads)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto font = load_text_font(16);
    IGNORE_USE_IN_ESCAPING_LAMBDA Array<float, 4> scales { 1, 1.5f, 2, 3 };

    // The memo answers the way a fresh query does: ask, displace it with another scale, ask again.
    auto fresh = font->skia_font(1);
    (void)font->skia_font(2);
    auto memoized = font->skia_font(1);
    EXPECT_EQ(memoized.getHinting(), fresh.getHinting());
    EXPECT_EQ(memoized.isForceAutoHinting(), fresh.isForceAutoHinting());

    IGNORE_USE_IN_ESCAPING_LAMBDA Array<Array<SkFont, 4>, 8> fonts;
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t thread_index = 0; thread_index < fonts.size(); ++thread_index) {
        auto thread = Threading::Thread::construct("HintingMemo"sv, [&font, &scales, &fonts, thread_index]() {
            for (size_t scale_index = 0; scale_index < scales.size(); ++scale_index)
                fonts[thread_index][scale_index] = font->skia_font(scales[scale_index]);
            return 0;
        });
        thread->start();
        threads.append(move(thread));
    }
    for (auto& thread : threads)
        (void)thread->join();

    for (auto const& thread_fonts : fonts) {
        for (size_t scale_index = 0; scale_index < scales.size(); ++scale_index) {
            EXPECT_EQ(thread_fonts[scale_index].getHinting(), fonts[0][scale_index].getHinting());
            EXPECT_EQ(thread_fonts[scale_index].isForceAutoHinting(), fonts[0][scale_index].isForceAutoHinting());
            EXPECT_EQ(thread_fonts[scale_index].getSize(), font->pixel_size() * scales[scale_index]);
        }
    }
}

// Only ThreadSanitizer can catch a cache race here, since every thread computes the same glyph IDs.
TEST_CASE(glyph_pages_can_be_populated_on_several_threads)
{
    auto font = load_text_font(16);
    NonnullRefPtr<Gfx::Typeface const> typeface { font->typeface() };
    IGNORE_USE_IN_ESCAPING_LAMBDA Array<u32, 4> code_points { 'A', 0x3a9, 0x4e00, 0x1f600 };

    IGNORE_USE_IN_ESCAPING_LAMBDA Array<Array<u32, 4>, 8> glyph_ids;
    IGNORE_USE_IN_ESCAPING_LAMBDA ConcurrentStart start;
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t thread_index = 0; thread_index < glyph_ids.size(); ++thread_index) {
        auto thread = Threading::Thread::construct("GlyphPageCache"sv, [typeface, &code_points, &glyph_ids, &start, thread_index]() {
            start.wait();
            for (size_t code_point_index = 0; code_point_index < code_points.size(); ++code_point_index)
                glyph_ids[thread_index][code_point_index] = typeface->glyph_id_for_code_point(code_points[code_point_index]);
            return 0;
        });
        thread->start();
        threads.append(move(thread));
    }
    for (auto& thread : threads)
        (void)thread->join();

    Array<u32, 4> expected_glyph_ids;
    for (size_t i = 0; i < code_points.size(); ++i)
        expected_glyph_ids[i] = typeface->glyph_id_for_code_point(code_points[i]);

    for (auto const& thread_glyph_ids : glyph_ids)
        EXPECT_EQ(thread_glyph_ids, expected_glyph_ids);
}

TEST_CASE(glyph_page_caches_keep_the_typefaces_in_use_once_full)
{
    auto file = MUST(Core::MappedFile::map(TEST_INPUT("fonts/text.ttf"sv)));
    IGNORE_USE_IN_ESCAPING_LAMBDA Vector<NonnullRefPtr<Gfx::Typeface>> typefaces;
    for (size_t i = 0; i < 200; ++i)
        typefaces.append(MUST(Gfx::Typeface::try_load_from_temporary_memory(file->bytes())));

    // A fresh thread starts with no caches. More typefaces than it keeps caches for fill them up, and a few typefaces
    // used in turn afterwards fill their glyph pages in once each.
    IGNORE_USE_IN_ESCAPING_LAMBDA u64 pages_populated_in_turns = 0;
    auto thread = Threading::Thread::construct("GlyphPageCache"sv, [&typefaces, &pages_populated_in_turns]() {
        for (auto const& typeface : typefaces)
            (void)typeface->glyph_id_for_code_point('A');
        auto populated_before_turns = Gfx::TypefaceSkia::glyph_pages_populated_on_this_thread();
        for (size_t turn = 0; turn < 50; ++turn) {
            for (size_t i = 0; i < 8; ++i)
                (void)typefaces[i * 20]->glyph_id_for_code_point('A');
        }
        pages_populated_in_turns = Gfx::TypefaceSkia::glyph_pages_populated_on_this_thread() - populated_before_turns;
        return 0;
    });
    thread->start();
    (void)thread->join();
    EXPECT(pages_populated_in_turns <= 8);
}

TEST_CASE(font_collection_preserves_each_face_style)
{
    auto file = MUST(Core::MappedFile::map(TEST_INPUT("fonts/styles.ttc"sv)));
    for (u32 index = 0; index < 3; ++index) {
        auto result = Gfx::TypefaceSkia::try_load_from_temporary_memory(file->bytes(), index);
        EXPECT(!result.is_error());
        if (result.is_error())
            continue;
        auto typeface = result.release_value();
        auto expected_weight = index == 1 ? 700u : 400u;
        auto expected_slope = index == 2 ? 1u : 0u;
        EXPECT_EQ(typeface->weight(), expected_weight);
        EXPECT_EQ(typeface->slope(), expected_slope);
        EXPECT_EQ(typeface->collection_index(), index);
        Gfx::FontVariationSettings variations;
        variations.set_weight(expected_weight);
        variations.set_width(100);
        variations.set_optical_sizing(16);
        auto font = typeface->font(12, variations);
        EXPECT_EQ(font->weight(), expected_weight);
        EXPECT_EQ(font->slope(), expected_slope);
        EXPECT_NE(font->glyph_id_for_code_point('a'), 0u);
    }
    EXPECT(Gfx::TypefaceSkia::try_load_from_temporary_memory(file->bytes(), 3).is_error());
}

TEST_CASE(font_collection_retains_shared_backing_for_skia)
{
    auto mapping = MUST(Core::MappedFile::map(TEST_INPUT("fonts/styles.ttc"sv)));
    auto backing = make_ref_counted<Gfx::Typeface::FontDataBacking>(move(mapping));
    sk_sp<SkTypeface const> skia_typeface;
    ByteBuffer expected_table;
    constexpr auto cmap_tag = SkSetFourByteTag('c', 'm', 'a', 'p');
    {
        auto const& mapping = backing->storage.get<NonnullOwnPtr<Core::MappedFile>>();

        auto typeface = MUST(Gfx::TypefaceSkia::load_from_buffer(mapping->bytes(), 1, backing));
        EXPECT_EQ(typeface->buffer().data(), mapping->bytes().data());
        skia_typeface = sk_ref_sp(typeface->sk_typeface());
        expected_table = MUST(ByteBuffer::create_uninitialized(skia_typeface->getTableSize(cmap_tag)));
        EXPECT(!expected_table.is_empty());
        EXPECT_EQ(skia_typeface->getTableData(cmap_tag, 0, expected_table.size(), expected_table.data()), expected_table.size());
    }
    // Skia still uses the original mapping after the Ladybird typeface has gone away.
    EXPECT(backing->ref_count() > 1);
    auto actual_table = MUST(ByteBuffer::create_uninitialized(expected_table.size()));
    EXPECT_EQ(skia_typeface->getTableData(cmap_tag, 0, actual_table.size(), actual_table.data()), actual_table.size());
    EXPECT_EQ(actual_table, expected_table);
}

// The answer depends on the installed font set alone, so every thread must reach the same one and
// the memo must match a code point once however many threads ask at the same moment.
TEST_CASE(system_fallback_fonts_can_be_matched_on_several_threads)
{
    Gfx::clear_system_fallback_font_cache();
    IGNORE_USE_IN_ESCAPING_LAMBDA Gfx::SystemFallbackFontKey key {
        .code_point = 0x4e2d,
        .weight = 400,
        .width = Gfx::FontWidth::Normal,
        .slope = 0,
        .prefer_color_emoji = false,
    };
    // NB: A machine without a font covering this code point answers null, and null is an answer the
    //     memo keeps like any other, so this test does not depend on what is installed.
    IGNORE_USE_IN_ESCAPING_LAMBDA Array<Gfx::Font const*, 8> matched {};
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t thread_index = 0; thread_index < matched.size(); ++thread_index) {
        auto thread = Threading::Thread::construct("SystemFallbackFont"sv, [&key, &matched, thread_index]() {
            matched[thread_index] = Gfx::system_fallback_font(key, 12).ptr();
            return 0;
        });
        thread->start();
        threads.append(move(thread));
    }
    for (auto& thread : threads)
        (void)thread->join();

    // One key is matched once, so every thread names the same font object, not eight equivalent ones.
    for (auto const* font : matched)
        EXPECT_EQ(font, matched[0]);
    EXPECT_EQ(Gfx::system_fallback_font_cache_size(), 1u);
    EXPECT_EQ(Gfx::system_fallback_font(key, 12).ptr(), matched[0]);

    // Another size picks another font from the same typeface, so it does not grow the memo.
    (void)Gfx::system_fallback_font(key, 13);
    EXPECT_EQ(Gfx::system_fallback_font_cache_size(), 1u);

    // A different style is a different question, not another answer to the same one.
    auto bold_key = key;
    bold_key.weight = 700;
    (void)Gfx::system_fallback_font(bold_key, 12);
    EXPECT_EQ(Gfx::system_fallback_font_cache_size(), 2u);
}

// A frozen cascade answers the same question as the live one, without entering the document.
TEST_CASE(frozen_cascade_matches_the_live_lookup)
{
    auto local_font = load_text_font(24);
    auto fallback_font = load_text_font(16);
    auto cascade = Gfx::FontCascadeList::create();
    cascade->add(local_font, { { 'a', 'a' } });
    cascade->add(fallback_font);
    cascade->set_last_resort_font(fallback_font);

    cascade->freeze();
    EXPECT(cascade->frozen_list());
    for (u32 code_point : { 'a', 'b', 'z' })
        EXPECT_EQ(&cascade->frozen_font_for_code_point(code_point), &cascade->font_for_code_point(code_point));
}

TEST_CASE(system_fallback_does_not_depend_on_earlier_lookups)
{
    auto first_font = load_text_font(16);
    auto second_font = load_text_font(24);
    auto cascade = Gfx::FontCascadeList::create();
    cascade->set_last_resort_font(first_font);
    cascade->set_system_font_fallback_callback([first_font, second_font](u32 code_point, Gfx::EmojiPresentation, Gfx::Font const&) -> RefPtr<Gfx::Font const> {
        return code_point == 'a' ? first_font : second_font;
    });

    EXPECT_EQ(&cascade->font_for_code_point('a'), first_font.ptr());
    EXPECT_EQ(&cascade->font_for_code_point('b'), second_font.ptr());
}

// https://drafts.csswg.org/css-fonts-4/#font-display-timeline
// A face in its block period renders invisibly and one in its swap period renders with the
// fallback, and the frozen cascade decides that from the period it recorded, not by resolving.
TEST_CASE(frozen_cascade_renders_a_pending_face_without_resolving_it)
{
    auto font = load_text_font(16);
    u32 resolves = 0;
    auto build = [&](Gfx::PendingFontState state) {
        auto cascade = Gfx::FontCascadeList::create();
        cascade->add_pending_face(
            { { 'a', 'a' } }, [&resolves, state] { ++resolves; return state; }, {}, [state] { return state; });
        cascade->add(font);
        cascade->set_last_resort_font(font);
        cascade->freeze();
        return cascade;
    };

    // Drop anything an earlier case left waiting, so the count below is only this case's.
    (void)Gfx::request_wanted_pending_faces();

    auto blocking = build(Gfx::PendingFontState::Invisible);
    EXPECT(blocking->frozen_font_for_code_point('a').is_invisible());
    EXPECT(!blocking->frozen_font_for_code_point('b').is_invisible());

    auto swapping = build(Gfx::PendingFontState::Visible);
    EXPECT_EQ(&swapping->frozen_font_for_code_point('a'), font.ptr());

    // Not one of those lookups resolved a face: the periods came from the snapshot.
    EXPECT_EQ(resolves, 0u);

    // Both faces are waiting for the document to request their loads, which is what starts them.
    EXPECT_EQ(Gfx::request_wanted_pending_faces(), 2u);
    EXPECT_EQ(resolves, 2u);
}

// A face whose display period has already failed contributes nothing and does not block the
// faces after it, so the frozen cascade does not carry it at all.
TEST_CASE(frozen_cascade_leaves_out_a_failed_pending_face)
{
    auto local_font = load_text_font(24);
    auto fallback_font = load_text_font(16);
    auto cascade = Gfx::FontCascadeList::create();
    cascade->add_pending_face(
        { { 'a', 'a' } }, [] { return Gfx::PendingFontState::Failed; }, {}, [] { return Gfx::PendingFontState::Failed; });
    cascade->add_pending_face(
        { { 'a', 'a' } }, [] { return Gfx::PendingFontState::Visible; }, [local_font] { return local_font; }, [] { return Gfx::PendingFontState::Visible; });
    cascade->add(fallback_font);
    cascade->set_last_resort_font(fallback_font);
    cascade->freeze();

    EXPECT_EQ(&cascade->frozen_font_for_code_point('a'), local_font.ptr());
}
