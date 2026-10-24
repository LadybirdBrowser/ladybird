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
#ifdef AK_OS_MACOS
#    include <LibGfx/Font/TypefaceCoreText.h>
#endif
#include <LibGfx/FontCascadeList.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

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
    return typeface->font(12)->is_emoji_font();
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

TEST_CASE(system_ui_italic_keeps_its_slope_with_variations)
{
    // A system UI font is one variable face for every style, so its typeface and its fonts answer with the style
    // that they were asked for, also after the typeface crosses IPC.
    float const font_size = 28;
    auto typeface = Gfx::TypefaceCoreText::system_ui({ Gfx::SystemUIFontKind::System, 400, Gfx::FontWidth::Normal, 1 });
    EXPECT(typeface);

    Gfx::FontVariationSettings variations;
    variations.set_weight(400);
    variations.set_width(100);
    variations.set_optical_sizing(font_size);
    auto font = typeface->font(font_size * 0.75f, variations);
    EXPECT_EQ(font->slope(), 1u);
    EXPECT_EQ(font->typeface().slope(), 1u);

    auto decoded = round_trip_typeface_through_ipc(font->typeface());
    EXPECT_EQ(decoded->slope(), 1u);
    EXPECT_EQ(decoded.ptr(), &font->typeface());
}
#endif

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
        auto populated_before_turns = Gfx::Typeface::glyph_pages_populated_on_this_thread();
        for (size_t turn = 0; turn < 50; ++turn) {
            for (size_t i = 0; i < 8; ++i)
                (void)typefaces[i * 20]->glyph_id_for_code_point('A');
        }
        pages_populated_in_turns = Gfx::Typeface::glyph_pages_populated_on_this_thread() - populated_before_turns;
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
        auto result = Gfx::Typeface::try_load_from_temporary_memory(file->bytes(), index);
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
    EXPECT(Gfx::Typeface::try_load_from_temporary_memory(file->bytes(), 3).is_error());
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
    IGNORE_USE_IN_ESCAPING_LAMBDA Array<RefPtr<Gfx::Font const>, 8> matched {};
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t thread_index = 0; thread_index < matched.size(); ++thread_index) {
        auto thread = Threading::Thread::construct("SystemFallbackFont"sv, [&key, &matched, thread_index]() {
            matched[thread_index] = Gfx::system_fallback_font(key, 12);
            return 0;
        });
        thread->start();
        threads.append(move(thread));
    }
    for (auto& thread : threads)
        (void)thread->join();

    // One key is matched once, so every thread names the same font object, not eight equivalent ones.
    for (auto const& font : matched)
        EXPECT_EQ(font.ptr(), matched[0].ptr());
    EXPECT_EQ(Gfx::system_fallback_font_cache_size(), 1u);
    EXPECT_EQ(Gfx::system_fallback_font(key, 12).ptr(), matched[0].ptr());

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

TEST_CASE(frozen_cascade_compares_pending_selection_snapshots)
{
    auto fallback = load_text_font(16);
    auto resident = load_text_font(24);
    u32 resolves = 0;
    auto build = [&](u64 source_face_id, Gfx::PendingFontState state, Vector<Gfx::UnicodeRange> ranges, RefPtr<Gfx::Font const> font = {}) {
        auto cascade = Gfx::FontCascadeList::create();
        cascade->add_pending_face(move(ranges), [&resolves, state] { ++resolves; return state; }, [font] { return font; }, [state] { return state; }, source_face_id);
        cascade->add(fallback);
        cascade->set_last_resort_font(fallback);
        cascade->freeze();
        return cascade;
    };
    auto first = build(1, Gfx::PendingFontState::Visible, { { 'a', 'a' } });
    auto same = build(1, Gfx::PendingFontState::Visible, { { 'a', 'a' } });
    EXPECT(first->equals(*first));
    EXPECT(first->equals(*same));
    EXPECT(!first->equals(*build(2, Gfx::PendingFontState::Visible, { { 'a', 'a' } })));
    EXPECT(!first->equals(*build(1, Gfx::PendingFontState::Invisible, { { 'a', 'a' } })));
    EXPECT(!first->equals(*build(1, Gfx::PendingFontState::Visible, { { 'b', 'b' } })));
    EXPECT(!first->equals(*build(1, Gfx::PendingFontState::Visible, { { 'a', 'a' } }, resident)));
    EXPECT(!build(0, Gfx::PendingFontState::Visible, { { 'a', 'a' } })->equals(*build(0, Gfx::PendingFontState::Visible, { { 'a', 'a' } })));

    auto loaded = Gfx::FontCascadeList::create();
    loaded->add(resident, { { 'a', 'a' } });
    loaded->add(fallback);
    loaded->set_last_resort_font(fallback);
    loaded->freeze();
    EXPECT(loaded->equals(*build(1, Gfx::PendingFontState::Visible, { { 'a', 'a' } }, resident)));
    EXPECT_EQ(resolves, 0u);
}

TEST_CASE(frozen_cascade_comparison_distinguishes_resident_faces_during_fallback)
{
    auto fallback = load_text_font(16);
    auto resident = load_text_font(24);
    for (auto state : { Gfx::PendingFontState::Visible, Gfx::PendingFontState::Invisible }) {
        auto build = [&](bool completed) {
            auto cascade = Gfx::FontCascadeList::create();
            cascade->add_pending_face({ { 'a', 'a' } }, [state] { return state; }, {}, [state] { return state; }, 1);
            if (completed)
                cascade->add(resident, { { 'a', 'a' } });
            else
                cascade->add_pending_face({ { 'a', 'a' } }, [] { return Gfx::PendingFontState::Visible; }, [resident] { return resident; }, [] { return Gfx::PendingFontState::Visible; }, 2);
            cascade->add(fallback);
            cascade->set_last_resort_font(fallback);
            cascade->freeze();
            return cascade;
        };
        auto before = build(false);
        auto after = build(true);
        EXPECT(before->equals(*build(false)));
        EXPECT(after->equals(*build(true)));
        EXPECT(!before->equals(*after));
        EXPECT(!after->equals(*before));
        EXPECT_EQ(before->frozen_font_for_code_point('a').point_size(), fallback->point_size());
        EXPECT_EQ(after->frozen_font_for_code_point('a').point_size(), resident->point_size());
        EXPECT_EQ(before->frozen_font_for_code_point('a').is_invisible(), state == Gfx::PendingFontState::Invisible);
        EXPECT_EQ(after->frozen_font_for_code_point('a').is_invisible(), state == Gfx::PendingFontState::Invisible);
    }
}

TEST_CASE(frozen_cascade_comparison_retains_font_display_state)
{
    auto fallback = load_text_font(16);
    auto state = Gfx::PendingFontState::Invisible;
    auto before = Gfx::FontCascadeList::create();
    before->add_pending_face({ { 'a', 'a' } }, [&state] { return state; }, {}, [&state] { return state; }, 1);
    before->add(fallback);
    before->set_last_resort_font(fallback);
    before->freeze();
    state = Gfx::PendingFontState::Visible;
    auto after = Gfx::FontCascadeList::create();
    after->extend(*before);
    after->set_last_resort_font(fallback);
    after->freeze();
    EXPECT(!before->equals(*after));
    EXPECT(before->frozen_font_for_code_point('a').is_invisible());
    EXPECT(!after->frozen_font_for_code_point('a').is_invisible());
}

TEST_CASE(frozen_cascade_comparison_includes_fallback_fonts)
{
    auto first_font = load_text_font(16);
    auto second_font = load_text_font(24);
    auto first = Gfx::FontCascadeList::create();
    first->set_last_resort_font(first_font);
    first->freeze();
    auto second = Gfx::FontCascadeList::create();
    second->set_last_resort_font(second_font);
    second->freeze();
    EXPECT(!first->equals(*second));

    auto build = [&](NonnullRefPtr<Gfx::Font> font) {
        auto cascade = Gfx::FontCascadeList::create();
        cascade->set_last_resort_font(first_font);
        auto fallback = Gfx::FontCascadeList::create();
        fallback->add(font);
        cascade->extend_fallback(*fallback);
        cascade->freeze();
        return cascade;
    };
    EXPECT(!build(first_font)->equals(*build(second_font)));
    EXPECT(build(first_font)->equals(*build(first_font)));
}
