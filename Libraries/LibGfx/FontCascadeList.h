/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/UnicodeRange.h>

namespace Gfx {

enum class EmojiPresentation : u8 {
    Text,
    Emoji,
};

enum class ForcedPresentation : u8 {
    No,
    Yes,
};

struct EmojiPresentationResult {
    EmojiPresentation presentation { EmojiPresentation::Text };
    ForcedPresentation forced { ForcedPresentation::No };
};

EmojiPresentationResult emoji_presentation_for_code_point(u32 code_point, Optional<u32> next_code_point);

enum class PendingFontState : u8 {
    Invisible,
    Visible,
    Failed,
};

// Requests the loads that render passes wanted while looking code points up in frozen cascades, and answers how many
// there were. Call this on the document thread once a pass has ended: resolving a face starts its fetch, arms its
// font-display timer and engages the document's load-event delayer.
size_t request_wanted_pending_faces();

// NB: The reference count is atomic because the Rust style engine's evaluation step takes and
//     gives up references to a resolved cascade list while building a font style group, and that
//     step runs on a style worker. Destruction stays on the main
//     thread: the engine's font-resolution cache holds one reference per resolution for the whole
//     transaction, so no worker can perform the final release.

class FontCascadeList : public AtomicRefCounted<FontCascadeList> {
public:
    using SystemFontFallbackCallback = Function<RefPtr<Font const>(u32, EmojiPresentation, Font const&)>;

    static NonnullRefPtr<FontCascadeList> create()
    {
        return adopt_ref(*new FontCascadeList());
    }

    bool is_empty() const { return m_fonts.is_empty() && m_pending_faces.is_empty() && !m_last_resort_font; }
    bool has_pending_faces() const { return !m_pending_faces.is_empty(); }
    Font const& first() const { return !m_fonts.is_empty() ? *m_fonts.first().font : *m_last_resort_font; }

    template<typename Callback>
    void for_each_font_entry(Callback callback) const
    {
        for (auto const& font : m_fonts)
            callback(font);
    }

    void add(NonnullRefPtr<Font const> font);
    void add(NonnullRefPtr<Font const> font, Vector<UnicodeRange> unicode_ranges);

    // Resolve a pending face only when it is selected for a rendered code point. `peek_state` answers the same question
    // as `resolve` without the side effects resolving has, so a frozen snapshot can record the face's display period
    // without starting its load; when it is absent, `resolve` answers, which only unit tests rely on.
    void add_pending_face(Vector<UnicodeRange> unicode_ranges, Function<PendingFontState()> resolve, Function<RefPtr<Font const>()> resolved_font = {}, Function<PendingFontState()> peek_state = {});

    void extend(FontCascadeList const& other);

    void extend_fallback(FontCascadeList const& other);

    Gfx::Font const& first_available_font() const;
    Gfx::Font const& font_for_code_point(u32 code_point, EmojiPresentationResult = {}) const;

    bool equals(FontCascadeList const& other) const;

    // One entry of the cascade, flattened into the order font_for_code_point() visits it.
    struct SnapshotEntry {
        // The font to use, or null for a pending face that has not produced one yet.
        Font const* font { nullptr };
        // The face's `unicode-range`; empty means the entry covers every code point.
        ReadonlySpan<UnicodeRange> unicode_ranges;
        // Zero unless this entry is a pending face waiting on a load.
        u64 pending_face_id { 0 };
        PendingFontState pending_state { PendingFontState::Visible };
    };

    // The cascade as a frozen render input: no caches to fill, no faces to resolve. A pending face whose display
    // period has already failed is left out, exactly as the lookup skips it.
    [[nodiscard]] Vector<SnapshotEntry> snapshot_entries() const;
    [[nodiscard]] Vector<SnapshotEntry> snapshot_fallback_entries() const;
    [[nodiscard]] Font const* last_resort_font() const { return m_last_resort_font.ptr(); }
    [[nodiscard]] bool has_system_font_fallback_callback() const { return !!m_system_font_fallback_callback; }

    // Builds the frozen list this cascade publishes to the render pipeline. Called once, after the cascade is complete
    // and before anything can look a code point up through the frozen list.
    void freeze();
    // The frozen list, or null for a cascade that was never published (canvas, unit tests).
    [[nodiscard]] void const* frozen_list() const { return m_frozen_list; }
    // The frozen list's answer for a code point. The cascade must have been frozen.
    [[nodiscard]] Font const& frozen_font_for_code_point(u32 code_point, EmojiPresentationResult = {}) const;

    struct Entry {
        NonnullRefPtr<Font const> font;
        struct RangeData {
            // The enclosing range is the union of all Unicode ranges. Used for fast skipping.
            UnicodeRange enclosing_range;

            Vector<UnicodeRange> unicode_ranges;

            bool operator==(RangeData const&) const = default;
        };
        Optional<RangeData> range_data;
    };

    class PendingFace : public AtomicRefCounted<PendingFace> {
    public:
        PendingFace(UnicodeRange enclosing, Vector<UnicodeRange> ranges, Function<PendingFontState()> resolve, Function<RefPtr<Font const>()> resolved_font, Function<PendingFontState()> peek_state);
        ~PendingFace();

        // The face a frozen cascade names, if it is still alive. A render pass only ever carries the number, which the
        // document turns back into a face once the pass has ended.
        [[nodiscard]] static RefPtr<PendingFace> with_id(u64);
        [[nodiscard]] u64 id() const { return m_id; }

        [[nodiscard]] ReadonlySpan<UnicodeRange> unicode_ranges() const { return m_unicode_ranges; }

        bool covers(u32 code_point) const
        {
            if (!m_enclosing_range.contains(code_point))
                return false;
            for (auto const& range : m_unicode_ranges) {
                if (range.contains(code_point))
                    return true;
            }
            return false;
        }

        PendingFontState resolve() const { return m_resolve(); }
        // What resolve() would answer, without starting a load or a display-period timer.
        PendingFontState peek_state() const { return m_peek_state ? m_peek_state() : m_resolve(); }
        Font const* resolved_font() const
        {
            if (!m_font && m_resolved_font)
                m_font = m_resolved_font();
            return m_font.ptr();
        }

    private:
        UnicodeRange m_enclosing_range;
        Vector<UnicodeRange> m_unicode_ranges;
        Function<PendingFontState()> m_resolve;
        Function<RefPtr<Font const>()> m_resolved_font;
        Function<PendingFontState()> m_peek_state;
        mutable RefPtr<Font const> m_font;
        u64 m_id { 0 };
    };

    void set_last_resort_font(NonnullRefPtr<Font> font)
    {
        m_first_available_font_cache = nullptr;
        m_last_resort_font = move(font);
    }
    void set_system_font_fallback_callback(SystemFontFallbackCallback callback) { m_system_font_fallback_callback = move(callback); }

    ~FontCascadeList();

private:
    Vector<SnapshotEntry> snapshot_entries(Vector<Entry> const& fonts, bool include_pending_faces) const;

    RefPtr<Font const> m_last_resort_font;
    mutable Vector<Entry> m_fonts;
    Vector<Entry> m_fallback_fonts;
    mutable HashTable<NonnullRefPtr<Font const>> m_retained_system_fallback_fonts;
    struct PendingEntry {
        size_t font_index;
        NonnullRefPtr<PendingFace> face;
    };
    Vector<PendingEntry> m_pending_faces;
    mutable HashMap<Font const*, NonnullRefPtr<Font>> m_invisible_fonts;
    SystemFontFallbackCallback m_system_font_fallback_callback;

    // OPTIMIZATION: Cache of resolved fonts for ASCII code points. Since m_fonts only grows and the cascade returns
    //               the first matching font, a cached hit can never become stale.
    mutable Array<Font const*, 128> m_ascii_cache {};

    // This cannot share m_ascii_cache because the first available font does not need to contain a space glyph.
    mutable Font const* m_first_available_font_cache { nullptr };

    // An owned `Arc<FrozenFontList>` from libgfx_rust, or null. Written once by freeze().
    void const* m_frozen_list { nullptr };
};

}
