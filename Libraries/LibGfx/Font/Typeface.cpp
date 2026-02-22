/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <harfbuzz/hb.h>

#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontVariationSettings.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/TypefaceSkia.h>

namespace Gfx {

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_resource(Core::Resource const& resource, u32 ttc_index)
{
    auto font_data = Gfx::FontData::create_from_resource(resource);
    return try_load_from_font_data(move(font_data), ttc_index);
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_font_data(NonnullOwnPtr<Gfx::FontData> font_data, u32 ttc_index)
{
    auto typeface = TRY(try_load_from_externally_owned_memory(font_data->bytes(), ttc_index));
    typeface->m_font_data = move(font_data);
    return typeface;
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_temporary_memory(ReadonlyBytes bytes, u32 ttc_index)
{
    auto buffer = TRY(ByteBuffer::copy(bytes));
    auto font_data = FontData::create_from_byte_buffer(move(buffer));
    return try_load_from_font_data(move(font_data), ttc_index);
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_externally_owned_memory(ReadonlyBytes bytes, u32 ttc_index)
{
    return TypefaceSkia::load_from_buffer(bytes, ttc_index);
}

Typeface::Typeface() = default;

Typeface::~Typeface()
{
    if (m_harfbuzz_face)
        hb_face_destroy(m_harfbuzz_face);
    if (m_harfbuzz_blob)
        hb_blob_destroy(m_harfbuzz_blob);
}

NonnullRefPtr<Font> Typeface::font(float point_size, FontVariationSettings const& variations, Gfx::ShapeFeatures const& shape_features) const
{
    FontCacheKey key { point_size, variations.to_sorted_list(), shape_features };

    if (auto it = m_fonts.find(key); it != m_fonts.end())
        return *it->value;

    // FIXME: It might be nice to have a global cap on the number of fonts we cache
    //        instead of doing it at the per-Typeface level like this.
    constexpr size_t max_cached_font_size_count = 128;
    if (m_fonts.size() > max_cached_font_size_count)
        m_fonts.remove(m_fonts.begin());

    RefPtr<Typeface const> used_typeface = const_cast<Typeface*>(this);
    if (!variations.is_empty()) {
        if (auto const* skia_typeface = as_if<TypefaceSkia const>(this))
            if (auto derived = skia_typeface->clone_with_variations(variations.to_sorted_list()))
                used_typeface = move(derived);
    }

    auto font = adopt_ref(*new Font(*used_typeface, point_size, point_size, DEFAULT_DPI, DEFAULT_DPI, variations, shape_features));
    m_fonts.set(key, font);
    return font;
}

hb_face_t* Typeface::harfbuzz_typeface() const
{
    if (!m_harfbuzz_blob)
        m_harfbuzz_blob = hb_blob_create(reinterpret_cast<char const*>(buffer().data()), buffer().size(), HB_MEMORY_MODE_READONLY, nullptr, [](void*) { });
    if (!m_harfbuzz_face)
        m_harfbuzz_face = hb_face_create(m_harfbuzz_blob, ttc_index());
    return m_harfbuzz_face;
}

}
