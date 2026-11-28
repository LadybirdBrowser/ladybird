/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/Singleton.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/RustFFI.h>
#include <LibUnicode/CharacterTypes.h>

namespace Gfx {

namespace {

// The pending faces a frozen cascade can name, by number. A render pass never touches a face: the frozen cascade it
// reads carries only the number, and the document turns numbers back into faces after the pass has ended. Faces are
// still created and destroyed wherever a cascade is built or let go of, which need not be the document thread.
struct PendingFaceRegistry {
    AK_ALLOC_WITH_KMALLOC;

    Mutex mutex;
    HashMap<u64, FontCascadeList::PendingFace*> faces;
};

Singleton<PendingFaceRegistry> s_pending_face_registry;

Atomic<u64> s_next_pending_face_id { 1 };

}

FontCascadeList::PendingFace::PendingFace(UnicodeRange enclosing, Vector<UnicodeRange> ranges, Function<PendingFontState()> resolve, Function<RefPtr<Font const>()> resolved_font, Function<PendingFontState()> peek_state, u64 source_face_id)
    : m_enclosing_range(enclosing)
    , m_unicode_ranges(move(ranges))
    , m_resolve(move(resolve))
    , m_resolved_font(move(resolved_font))
    , m_peek_state(move(peek_state))
    , m_id(s_next_pending_face_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed))
    , m_source_face_id(source_face_id)
{
    MutexLocker locker(s_pending_face_registry->mutex);
    s_pending_face_registry->faces.set(m_id, this);
}

FontCascadeList::PendingFace::~PendingFace()
{
    MutexLocker locker(s_pending_face_registry->mutex);
    s_pending_face_registry->faces.remove(m_id);
}

RefPtr<FontCascadeList::PendingFace> FontCascadeList::PendingFace::with_id(u64 id)
{
    MutexLocker locker(s_pending_face_registry->mutex);
    auto face = s_pending_face_registry->faces.get(id);
    if (!face.has_value())
        return nullptr;
    // NB: A face whose last reference is being dropped on another thread stays registered until its destructor takes
    //     the mutex, so it must not be revived.
    if (!(*face)->try_ref())
        return nullptr;
    return adopt_ref(**face);
}

size_t request_wanted_pending_faces()
{
    return FFI::ladybird_gfx_request_wanted_pending_faces();
}

FontCascadeList::~FontCascadeList()
{
    if (m_frozen_list)
        FFI::ladybird_gfx_frozen_font_list_release(m_frozen_list);
}

EmojiPresentationResult emoji_presentation_for_code_point(u32 code_point, Optional<u32> next_code_point, Optional<u32> code_point_after_next)
{
    // VARIATION SELECTOR-16 (emoji)
    if (next_code_point == 0xFE0Fu)
        return { EmojiPresentation::Emoji, ForcedPresentation::Yes };
    // VARIATION SELECTOR-15 (text)
    if (next_code_point == 0xFE0Eu)
        return { EmojiPresentation::Text, ForcedPresentation::Yes };

    // https://www.unicode.org/reports/tr51/#Diversity
    // "Emoji presentation selectors are neither needed nor recommended for emoji characters when they are followed by
    //  emoji modifiers, and should not be used in newly generated emoji modifier sequences; the emoji modifier
    //  automatically implies the emoji presentation style."
    if (next_code_point.has_value() && *next_code_point >= 0x1F3FBu && *next_code_point <= 0x1F3FFu && Unicode::code_point_has_emoji_modifier_base_property(code_point))
        return { EmojiPresentation::Emoji, ForcedPresentation::Yes };

    // https://www.unicode.org/reports/tr51/#def_emoji_zwj_sequence
    // NB: A ZERO WIDTH JOINER between two pictographic characters joins them into an emoji ZWJ sequence, which is one
    //     emoji, so it takes emoji presentation even when the first character's own default is text, as in U+1F441
    //     U+200D U+1F5E8. A joiner that no pictographic character follows joins nothing.
    if (next_code_point == 0x200Du && code_point_after_next.has_value()
        && Unicode::code_point_has_extended_pictographic_property(code_point)
        && Unicode::code_point_has_extended_pictographic_property(*code_point_after_next))
        return { EmojiPresentation::Emoji, ForcedPresentation::Yes };

    if (Unicode::code_point_has_emoji_presentation_property(code_point))
        return { EmojiPresentation::Emoji, ForcedPresentation::No };
    return { EmojiPresentation::Text, ForcedPresentation::No };
}

void FontCascadeList::add(NonnullRefPtr<Font const> font)
{
    m_first_available_font_cache = nullptr;
    m_fonts.append({ move(font), {} });
}

void FontCascadeList::add(NonnullRefPtr<Font const> font, Vector<UnicodeRange> unicode_ranges)
{
    m_first_available_font_cache = nullptr;
    if (unicode_ranges.is_empty()) {
        m_fonts.append({ move(font), {} });
        return;
    }
    u32 lowest_code_point = 0xFFFFFFFF;
    u32 highest_code_point = 0;

    for (auto& range : unicode_ranges) {
        lowest_code_point = min(lowest_code_point, range.min_code_point());
        highest_code_point = max(highest_code_point, range.max_code_point());
    }

    m_fonts.append({ move(font),
        Entry::RangeData {
            { lowest_code_point, highest_code_point },
            move(unicode_ranges),
        } });
}

void FontCascadeList::add_pending_face(Vector<UnicodeRange> unicode_ranges, Function<PendingFontState()> resolve, Function<RefPtr<Font const>()> resolved_font, Function<PendingFontState()> peek_state, u64 source_face_id)
{
    m_ascii_cache.fill(nullptr);
    if (unicode_ranges.is_empty())
        return;

    u32 lowest_code_point = 0xFFFFFFFF;
    u32 highest_code_point = 0;
    for (auto const& range : unicode_ranges) {
        lowest_code_point = min(lowest_code_point, range.min_code_point());
        highest_code_point = max(highest_code_point, range.max_code_point());
    }

    m_pending_faces.append({ m_fonts.size(), adopt_ref(*new PendingFace(UnicodeRange { lowest_code_point, highest_code_point }, move(unicode_ranges), move(resolve), move(resolved_font), move(peek_state), source_face_id)) });
}

void FontCascadeList::extend(FontCascadeList const& other)
{
    m_ascii_cache.fill(nullptr);
    m_first_available_font_cache = nullptr;
    for (auto const& pending : other.m_pending_faces)
        m_pending_faces.append({ m_fonts.size() + pending.font_index, pending.face });
    m_fonts.extend(other.m_fonts);
}

void FontCascadeList::extend_fallback(FontCascadeList const& other)
{
    m_fallback_fonts.extend(other.m_fonts);
}

// https://drafts.csswg.org/css-fonts/#first-available-font
Gfx::Font const& FontCascadeList::first_available_font() const
{
    if (m_first_available_font_cache && m_pending_faces.is_empty())
        return *m_first_available_font_cache;

    // The first available font, used for example in the definition of font-relative lengths such as ex or in the
    // definition of the line-height property, is defined to be the first font for which the character U+0020 (space)
    // is not excluded by a unicode-range, given the font families in the font-family list (or a user agent’s default
    // font if none are available).
    static constexpr u32 space_code_point = 0x20;

    size_t pending_index = 0;
    auto resolve_pending_faces = [&](size_t font_index) -> Font const* {
        while (pending_index < m_pending_faces.size()
            && m_pending_faces[pending_index].font_index <= font_index) {
            auto const& pending = m_pending_faces[pending_index++].face;
            if (!pending->covers(space_code_point))
                continue;
            // NB: Metric probes can use resident faces, but must not initiate font loads.
            if (auto* font = pending->resolved_font())
                return font;
        }
        return nullptr;
    };

    for (size_t font_index = 0; font_index < m_fonts.size(); ++font_index) {
        if (auto* font = resolve_pending_faces(font_index)) {
            m_first_available_font_cache = font;
            return *font;
        }
        auto const& entry = m_fonts[font_index];
        if (!entry.range_data.has_value()) {
            m_first_available_font_cache = entry.font.ptr();
            return *m_first_available_font_cache;
        }
        if (!entry.range_data->enclosing_range.contains(space_code_point))
            continue;

        for (auto const& range : entry.range_data->unicode_ranges) {
            if (range.contains(space_code_point)) {
                m_first_available_font_cache = entry.font.ptr();
                return *m_first_available_font_cache;
            }
        }
    }

    if (auto* font = resolve_pending_faces(m_fonts.size())) {
        m_first_available_font_cache = font;
        return *font;
    }

    m_first_available_font_cache = m_last_resort_font.ptr();
    return *m_first_available_font_cache;
}

Gfx::Font const& FontCascadeList::font_for_code_point(u32 code_point, EmojiPresentationResult emoji_presentation) const
{
    auto use_ascii_cache = code_point < m_ascii_cache.size() && emoji_presentation.presentation == EmojiPresentation::Text && emoji_presentation.forced == ForcedPresentation::No;
    if (use_ascii_cache) {
        if (auto const* cached = m_ascii_cache[code_point])
            return *cached;
    }

    bool invisible = false;
    bool pending_face_may_change = false;
    auto cache_and_return = [&](Font const& font) -> Font const& {
        auto const* selected_font = &font;
        if (invisible) {
            // https://drafts.csswg.org/css-fonts-4/#invisible-fallback
            // Create an anonymous font face with the same metrics as the selected font face
            // but with all glyphs "invisible" (containing no "ink"), and use that for rendering text.
            selected_font = m_invisible_fonts.ensure(&font, [&] { return font.invisible_variant(); }).ptr();
        }
        if (use_ascii_cache && !pending_face_may_change)
            m_ascii_cache[code_point] = selected_font;
        return *selected_font;
    };

    auto presentation_matches = [wants_emoji = emoji_presentation.presentation == EmojiPresentation::Emoji](Font const& font) {
        return font.is_emoji_font() == wants_emoji;
    };

    auto entry_contains_glyph = [code_point](Entry const& entry) {
        if (!entry.range_data.has_value())
            return entry.font->contains_glyph(code_point);
        if (!entry.range_data->enclosing_range.contains(code_point))
            return false;
        for (auto const& range : entry.range_data->unicode_ranges) {
            if (range.contains(code_point) && entry.font->contains_glyph(code_point))
                return true;
        }
        return false;
    };

    size_t pending_index = 0;
    bool rendering_with_fallback = false;
    Font const* author_glyph_match = nullptr;
    auto resolve_pending_faces = [&](size_t font_index) -> Font const* {
        while (!rendering_with_fallback && pending_index < m_pending_faces.size()
            && m_pending_faces[pending_index].font_index <= font_index) {
            auto const& pending = m_pending_faces[pending_index++].face;
            if (!pending->covers(code_point))
                continue;
            auto state = pending->resolve();
            if (state == PendingFontState::Failed)
                continue;
            // NB: A local face can become available during resolution. Use its glyphs on
            //     this first measurement without starting any unused fallback font loads.
            if (auto* font = pending->resolved_font()) {
                if (!font->contains_glyph(code_point))
                    continue;
                if (emoji_presentation.forced == ForcedPresentation::No || presentation_matches(*font))
                    return font;
                if (!author_glyph_match)
                    author_glyph_match = font;
                continue;
            }
            pending_face_may_change = true;
            invisible = state == PendingFontState::Invisible;
            // https://drafts.csswg.org/css-fonts-4/#font-display-timeline
            // Doing this must not trigger loads of any of the fallback fonts.
            rendering_with_fallback = true;
        }
        return nullptr;
    };

    for (size_t font_index = 0; font_index < m_fonts.size(); ++font_index) {
        if (auto* font = resolve_pending_faces(font_index))
            return cache_and_return(*font);
        auto const& entry = m_fonts[font_index];
        if (!entry_contains_glyph(entry))
            continue;
        if (emoji_presentation.forced == ForcedPresentation::No || presentation_matches(*entry.font))
            return cache_and_return(*entry.font);
        if (!author_glyph_match)
            author_glyph_match = entry.font.ptr();
    }

    if (auto* font = resolve_pending_faces(m_fonts.size()))
        return cache_and_return(*font);

    Font const* fallback_glyph_match = nullptr;
    for (auto const& entry : m_fallback_fonts) {
        if (!entry_contains_glyph(entry))
            continue;
        if (presentation_matches(*entry.font))
            return cache_and_return(*entry.font);
        if (!fallback_glyph_match)
            fallback_glyph_match = entry.font.ptr();
    }

    if (m_system_font_fallback_callback) {
        if (auto fallback = m_system_font_fallback_callback(code_point, emoji_presentation.presentation, m_system_fallback_request)) {
            if (presentation_matches(*fallback) || (!author_glyph_match && !fallback_glyph_match)) {
                auto const& fallback_font = *fallback;
                m_retained_system_fallback_fonts.set(fallback.release_nonnull());
                return cache_and_return(fallback_font);
            }
        }
    }

    if (author_glyph_match)
        return cache_and_return(*author_glyph_match);
    if (fallback_glyph_match)
        return cache_and_return(*fallback_glyph_match);

    return cache_and_return(*m_last_resort_font);
}

Vector<FontCascadeList::SnapshotEntry> FontCascadeList::snapshot_entries(Vector<Entry> const& fonts, bool include_pending_faces) const
{
    Vector<SnapshotEntry> entries;
    entries.ensure_capacity(fonts.size() + (include_pending_faces ? m_pending_faces.size() : 0));

    size_t pending_index = 0;
    auto take_pending_faces_up_to = [&](size_t font_index) {
        if (!include_pending_faces)
            return;
        while (pending_index < m_pending_faces.size() && m_pending_faces[pending_index].font_index <= font_index) {
            auto const& face = m_pending_faces[pending_index++].face;
            auto state = face->peek_state();
            // A face whose display period has failed never contributes a glyph and never blocks the faces after it,
            // so the frozen cascade simply does not carry it.
            if (state == PendingFontState::Failed)
                continue;
            entries.append({
                .font = face->resolved_font(),
                .unicode_ranges = face->unicode_ranges(),
                .pending_face_id = face->id(),
                .pending_state = state,
                .source_face_id = face->source_face_id(),
            });
        }
    };

    for (size_t font_index = 0; font_index < fonts.size(); ++font_index) {
        take_pending_faces_up_to(font_index);
        auto const& entry = fonts[font_index];
        entries.append({
            .font = entry.font.ptr(),
            .unicode_ranges = entry.range_data.has_value() ? entry.range_data->unicode_ranges.span() : ReadonlySpan<UnicodeRange> {},
        });
    }
    take_pending_faces_up_to(fonts.size());

    return entries;
}

Vector<FontCascadeList::SnapshotEntry> FontCascadeList::snapshot_entries() const
{
    return snapshot_entries(m_fonts, true);
}

Vector<FontCascadeList::SnapshotEntry> FontCascadeList::snapshot_fallback_entries() const
{
    return snapshot_entries(m_fallback_fonts, false);
}

// Gives the cascade to the callback as flat arrays, which are live for the call.
template<typename Callback>
static void with_flat_snapshot(FontCascadeList const& list, Callback callback)
{
    Vector<FFI::FfiCascadeSnapshotRange> ranges;
    Vector<FFI::FfiCascadeSnapshotRange> fallback_ranges;
    auto flatten = [](Vector<FontCascadeList::SnapshotEntry> const& entries, Vector<FFI::FfiCascadeSnapshotRange>& ranges) {
        Vector<FFI::FfiCascadeSnapshotEntry> flat_entries;
        flat_entries.ensure_capacity(entries.size());
        for (auto const& entry : entries) {
            flat_entries.unchecked_append({
                .font = entry.font,
                .range_offset = ranges.size(),
                .range_count = entry.unicode_ranges.size(),
                .pending_face_id = entry.pending_face_id,
                .pending_state = to_underlying(entry.pending_state),
                .source_face_id = entry.source_face_id,
            });
            for (auto const& range : entry.unicode_ranges)
                ranges.append({ range.min_code_point(), range.max_code_point() });
        }
        return flat_entries;
    };
    auto entries = flatten(list.snapshot_entries(), ranges);
    auto fallback_entries = flatten(list.snapshot_fallback_entries(), fallback_ranges);

    FFI::FfiCascadeSnapshot snapshot {
        .entries = entries.data(),
        .entry_count = entries.size(),
        .ranges = ranges.data(),
        .range_count = ranges.size(),
        .fallback_entries = fallback_entries.data(),
        .fallback_entry_count = fallback_entries.size(),
        .fallback_ranges = fallback_ranges.data(),
        .fallback_range_count = fallback_ranges.size(),
        .last_resort_font = list.last_resort_font(),
        .system_fallback_features = nullptr,
        .system_fallback_feature_count = 0,
        .system_fallback_point_size = 0,
        .system_fallback_weight = 0,
        .system_fallback_width = 0,
        .system_fallback_slope = 0,
        .has_system_fallback = list.has_system_font_fallback_callback(),
    };
    if (snapshot.has_system_fallback) {
        // The system fallback matches against the request of the cascade, as the lookup does.
        auto const& request = list.system_fallback_request();
        snapshot.system_fallback_features = reinterpret_cast<FFI::FfiShapeFeature const*>(request.shape_features.data());
        snapshot.system_fallback_feature_count = request.shape_features.size();
        snapshot.system_fallback_point_size = request.point_size;
        snapshot.system_fallback_weight = request.weight;
        snapshot.system_fallback_width = request.width;
        snapshot.system_fallback_slope = request.slope;
    }
    callback(snapshot);
}

void FontCascadeList::freeze()
{
    VERIFY(!m_frozen_list);
    with_flat_snapshot(*this, [&](FFI::FfiCascadeSnapshot const& snapshot) {
        m_frozen_list = FFI::ladybird_gfx_frozen_font_list_build(&snapshot);
    });
}

Font const& FontCascadeList::frozen_font_for_code_point(u32 code_point, EmojiPresentationResult presentation) const
{
    VERIFY(m_frozen_list);
    return *static_cast<Font const*>(FFI::ladybird_gfx_frozen_font_list_font_for_code_point(m_frozen_list,
        code_point, presentation.presentation == EmojiPresentation::Emoji, presentation.forced == ForcedPresentation::Yes));
}

bool FontCascadeList::equals(FontCascadeList const& other) const
{
    if (m_frozen_list && other.m_frozen_list)
        return FFI::ladybird_gfx_frozen_font_lists_equal(m_frozen_list, other.m_frozen_list);
    if (!m_pending_faces.is_empty() || !other.m_pending_faces.is_empty())
        return false;
    if (m_fonts.size() != other.m_fonts.size())
        return false;
    for (size_t i = 0; i < m_fonts.size(); ++i) {
        // NB: Two entries with the same font but different Unicode ranges pick different code points.
        if (m_fonts[i].font != other.m_fonts[i].font || m_fonts[i].range_data != other.m_fonts[i].range_data)
            return false;
    }
    return true;
}

bool FontCascadeList::fills_toward(FontCascadeList const& resolved) const
{
    VERIFY(m_frozen_list && resolved.m_frozen_list);
    return FFI::ladybird_gfx_font_cascade_list_fills_toward(this, &resolved);
}

}

extern "C" {
void const* ladybird_gfx_font_cascade_list_font_for_code_point(void const*, u32, bool, bool);
void const* ladybird_gfx_font_cascade_list_first(void const*);
void ladybird_gfx_font_cascade_list_ref(void const*);
void ladybird_gfx_font_cascade_list_unref(void const*);
u8 ladybird_gfx_emoji_presentation_for_code_point(u32, u32, bool, u32, bool);
void const* ladybird_gfx_font_cascade_list_frozen(void const*);
void ladybird_gfx_cascade_snapshot(void const*, void*, void (*)(void*, Gfx::FFI::FfiCascadeSnapshot const*));
bool ladybird_gfx_resolve_pending_face(u64);
}

extern "C" void const* ladybird_gfx_font_cascade_list_font_for_code_point(void const* list, u32 code_point, bool emoji_presentation, bool forced_presentation)
{
    VERIFY(list);
    auto const& cascade_list = *static_cast<Gfx::FontCascadeList const*>(list);
    return &cascade_list.font_for_code_point(
        code_point,
        { emoji_presentation ? Gfx::EmojiPresentation::Emoji : Gfx::EmojiPresentation::Text,
            forced_presentation ? Gfx::ForcedPresentation::Yes : Gfx::ForcedPresentation::No });
}

extern "C" void const* ladybird_gfx_font_cascade_list_first(void const* list)
{
    VERIFY(list);
    return &static_cast<Gfx::FontCascadeList const*>(list)->first();
}

extern "C" void ladybird_gfx_font_cascade_list_ref(void const* list)
{
    VERIFY(list);
    static_cast<Gfx::FontCascadeList const*>(list)->ref();
}

extern "C" void ladybird_gfx_font_cascade_list_unref(void const* list)
{
    VERIFY(list);
    static_cast<Gfx::FontCascadeList const*>(list)->unref();
}

extern "C" u8 ladybird_gfx_emoji_presentation_for_code_point(u32 code_point, u32 next_code_point, bool has_next_code_point, u32 code_point_after_next, bool has_code_point_after_next)
{
    auto result = Gfx::emoji_presentation_for_code_point(
        code_point,
        has_next_code_point ? Optional<u32> { next_code_point } : Optional<u32> {},
        has_code_point_after_next ? Optional<u32> { code_point_after_next } : Optional<u32> {});
    u8 encoded = 0;
    if (result.presentation == Gfx::EmojiPresentation::Emoji)
        encoded |= 1;
    if (result.forced == Gfx::ForcedPresentation::Yes)
        encoded |= 2;
    return encoded;
}

extern "C" void const* ladybird_gfx_font_cascade_list_frozen(void const* list)
{
    VERIFY(list);
    return static_cast<Gfx::FontCascadeList const*>(list)->frozen_list();
}

// NB: Rust reads the features of the system fallback request as its FfiShapeFeature.
static_assert(sizeof(Gfx::ShapeFeature) == sizeof(Gfx::FFI::FfiShapeFeature));
static_assert(offsetof(Gfx::ShapeFeature, value) == offsetof(Gfx::FFI::FfiShapeFeature, value));
static_assert(offsetof(Gfx::ShapeFeature, set_by_feature_settings) == offsetof(Gfx::FFI::FfiShapeFeature, set_by_feature_settings));

// Gives the cascade as it is now to `visit`, as flat arrays that are live for the call.
extern "C" void ladybird_gfx_cascade_snapshot(void const* list, void* context, void (*visit)(void*, Gfx::FFI::FfiCascadeSnapshot const*))
{
    VERIFY(list);
    Gfx::with_flat_snapshot(*static_cast<Gfx::FontCascadeList const*>(list), [&](Gfx::FFI::FfiCascadeSnapshot const& snapshot) {
        visit(context, &snapshot);
    });
}

extern "C" bool ladybird_gfx_resolve_pending_face(u64 id)
{
    auto face = Gfx::FontCascadeList::PendingFace::with_id(id);
    if (!face)
        return false;
    (void)face->resolve();
    return true;
}
