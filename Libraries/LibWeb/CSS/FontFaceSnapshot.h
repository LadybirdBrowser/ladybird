/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/Vector.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/UnicodeRange.h>
#include <LibGfx/FontCascadeList.h>
#include <LibWeb/CSS/FontComputer.h>

namespace Web::CSS {

// The typeface a face renders with, which the face publishes on the document thread as it loads or its font-display
// period fails, and which a cascade with the face pending reads on whichever thread it is laid out: the face itself is
// the document thread's alone.
class FontFaceRenderingTypeface final : public AtomicRefCounted<FontFaceRenderingTypeface> {
public:
    static NonnullRefPtr<FontFaceRenderingTypeface> create() { return adopt_ref(*new FontFaceRenderingTypeface); }

    [[nodiscard]] RefPtr<Gfx::Typeface const> get() const
    {
        MutexLocker locker(m_mutex);
        return m_typeface;
    }
    void set(RefPtr<Gfx::Typeface const> typeface)
    {
        MutexLocker locker(m_mutex);
        m_typeface = move(typeface);
    }

private:
    FontFaceRenderingTypeface() = default;

    mutable Mutex m_mutex;
    RefPtr<Gfx::Typeface const> m_typeface;
};

// The document's @font-face table at one font environment generation: what font matching reads of each face, and
// nothing that could reach the face itself. Immutable once built, so resolving a font from it cannot start a load or
// observe a face changing underneath it. The style engine holds a reference beside the document's, and may let go of
// it on whichever thread it runs on.
class FontFaceSnapshot final : public AtomicRefCounted<FontFaceSnapshot> {
public:
    struct Face {
        // The number the document knows the face by, for a resolution that wants it loaded.
        u64 id { 0 };
        // The loaded typeface, or null while the face is pending.
        RefPtr<Gfx::Typeface const> typeface;
        // What a cascade with the face pending renders with once the face has loaded.
        NonnullRefPtr<FontFaceRenderingTypeface const> rendering_typeface;
        // Where a pending face is on its font-display timeline, which a frozen cascade records.
        Gfx::PendingFontState rendering_state { Gfx::PendingFontState::Visible };
        Vector<Gfx::UnicodeRange> unicode_ranges;
        bool has_urls { false };
        // Its font-display period failed or its load errored, so it contributes nothing.
        bool is_unusable { false };
        bool has_non_default_unicode_range { false };
    };

    // Each key of the table and the faces registered under it, in order.
    using Table = OrderedHashMap<FontFaceKey, Vector<Face>>;

    static NonnullRefPtr<FontFaceSnapshot> create(u64 generation, Table table, NonnullRefPtr<FontFeatureValuesByScope const> font_feature_values) { return adopt_ref(*new FontFaceSnapshot(generation, move(table), move(font_feature_values))); }

    [[nodiscard]] u64 generation() const { return m_generation; }

    // In the order the live table iterates them, which is the order font matching meets them in.
    [[nodiscard]] Table const& table() const { return m_table; }
    [[nodiscard]] Vector<Face> const* faces(FontFaceKey const& key) const
    {
        auto it = m_table.find(key);
        return it == m_table.end() ? nullptr : &it->value;
    }

    // The @font-feature-values an element of a tree scope that declares some, or of the document's, sees for a family,
    // which font-variant-alternates names its features through. They change the generation as faces do.
    [[nodiscard]] FontFeatureValues const& font_feature_values(TreeScopeID tree_scope, Utf16FlyString const& family) const
    {
        static NeverDestroyed<FontFeatureValues const> none;
        auto scope = m_font_feature_values->scopes.find(tree_scope);
        if (scope == m_font_feature_values->scopes.end())
            return *none;
        auto it = scope->value.find(family);
        return it == scope->value.end() ? *none : it->value;
    }
    // The shadow tree scopes that declare @font-feature-values of their own.
    [[nodiscard]] ReadonlySpan<TreeScopeID> font_feature_values_shadow_scopes() const { return m_font_feature_values->shadow_scopes; }

private:
    FontFaceSnapshot(u64 generation, Table table, NonnullRefPtr<FontFeatureValuesByScope const> font_feature_values)
        : m_generation(generation)
        , m_table(move(table))
        , m_font_feature_values(move(font_feature_values))
    {
    }

    u64 m_generation { 0 };
    Table m_table;
    NonnullRefPtr<FontFeatureValuesByScope const> m_font_feature_values;
};

}
