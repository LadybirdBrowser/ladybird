/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/UnicodeRange.h>
#include <LibWeb/CSS/FontComputer.h>

namespace Web::CSS {

// The document's @font-face table at one font environment generation: what font matching reads of each face, and
// nothing that could reach the face itself. Immutable once built, so resolving a font from it cannot start a load or
// observe a face changing underneath it.
class FontFaceSnapshot final : public RefCounted<FontFaceSnapshot> {
public:
    struct Face {
        // The number the document knows the face by, for a resolution that wants it loaded.
        u64 id { 0 };
        // The loaded typeface, or null while the face is pending.
        RefPtr<Gfx::Typeface const> typeface;
        Vector<Gfx::UnicodeRange> unicode_ranges;
        bool has_urls { false };
        // Its font-display period failed or its load errored, so it contributes nothing.
        bool is_unusable { false };
        bool has_non_default_unicode_range { false };
    };

    // Each key of the table and the faces registered under it, in order.
    using Table = OrderedHashMap<FontFaceKey, Vector<Face>>;

    static NonnullRefPtr<FontFaceSnapshot> create(u64 generation, Table table, NonnullRefPtr<FontFeatureValuesByFamily const> font_feature_values) { return adopt_ref(*new FontFaceSnapshot(generation, move(table), move(font_feature_values))); }

    [[nodiscard]] u64 generation() const { return m_generation; }

    // In the order the live table iterates them, which is the order font matching meets them in.
    [[nodiscard]] Table const& table() const { return m_table; }
    [[nodiscard]] Vector<Face> const* faces(FontFaceKey const& key) const
    {
        auto it = m_table.find(key);
        return it == m_table.end() ? nullptr : &it->value;
    }

    // The @font-feature-values of the document's tree scope for a family, which font-variant-alternates names its
    // features through. They change the generation as faces do.
    [[nodiscard]] FontFeatureValues const& font_feature_values(Utf16FlyString const& family) const
    {
        static NeverDestroyed<FontFeatureValues const> none;
        auto it = m_font_feature_values->families.find(family);
        return it == m_font_feature_values->families.end() ? *none : it->value;
    }

private:
    FontFaceSnapshot(u64 generation, Table table, NonnullRefPtr<FontFeatureValuesByFamily const> font_feature_values)
        : m_generation(generation)
        , m_table(move(table))
        , m_font_feature_values(move(font_feature_values))
    {
    }

    u64 m_generation { 0 };
    Table m_table;
    NonnullRefPtr<FontFeatureValuesByFamily const> m_font_feature_values;
};

}
