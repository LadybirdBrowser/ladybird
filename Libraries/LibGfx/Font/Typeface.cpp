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
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Gfx {

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_resource(Core::Resource const& resource, u32 ttc_index)
{
    auto typeface = TRY(try_load_from_externally_owned_memory(resource.data(), ttc_index));
    typeface->set_resource_font_data(resource);
    return typeface;
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_anonymous_buffer(Core::AnonymousBuffer anonymous_buffer, u32 ttc_index)
{
    auto typeface = TRY(try_load_from_externally_owned_memory(anonymous_buffer.bytes(), ttc_index));
    typeface->set_anonymous_font_data(move(anonymous_buffer));
    return typeface;
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::try_load_from_temporary_memory(ReadonlyBytes bytes, u32 ttc_index)
{
    auto anonymous_buffer = TRY(Core::AnonymousBuffer::create_with_size(bytes.size()));
    if (!bytes.is_empty())
        memcpy(anonymous_buffer.data<void>(), bytes.data(), bytes.size());
    return try_load_from_anonymous_buffer(move(anonymous_buffer), ttc_index);
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

    auto font = adopt_ref(*new Font(*used_typeface, point_size, point_size, variations, shape_features));
    m_fonts.set(key, font);
    return font;
}

hb_face_t* Typeface::harfbuzz_typeface() const
{
    if (!m_harfbuzz_face)
        m_harfbuzz_face = create_harfbuzz_face();
    return m_harfbuzz_face;
}

hb_face_t* Typeface::create_harfbuzz_face() const
{
    if (!m_harfbuzz_blob)
        m_harfbuzz_blob = hb_blob_create(reinterpret_cast<char const*>(buffer().data()), buffer().size(), HB_MEMORY_MODE_READONLY, nullptr, [](void*) { });
    return hb_face_create(m_harfbuzz_blob, ttc_index());
}

void Typeface::encode_font_data_for_ipc(IPC::Encoder& encoder) const
{
    VERIFY(m_font_data.has_value());

    m_font_data->visit(
        [&](Core::AnonymousBuffer const& anonymous_buffer) {
            MUST(encoder.encode(FontDataFormat::RawFontData));
            MUST(encoder.encode(anonymous_buffer));
            MUST(encoder.encode(ttc_index()));
        },
        [&](NonnullRefPtr<Core::Resource const> const& resource) {
            MUST(encoder.encode(FontDataFormat::ResourceFontData));
            MUST(encoder.encode(resource->uri()));
            MUST(encoder.encode(ttc_index()));
        });
}

void Typeface::set_anonymous_font_data(Core::AnonymousBuffer anonymous_buffer)
{
    m_font_data = FontDataBacking { move(anonymous_buffer) };
}

void Typeface::set_resource_font_data(Core::Resource const& resource)
{
    m_font_data = FontDataBacking { NonnullRefPtr<Core::Resource const> { resource } };
}

void Typeface::copy_font_data_from(Typeface const& other)
{
    m_font_data = other.m_font_data;
}

}

namespace IPC {

static NonnullRefPtr<Gfx::Typeface const> match_system_typeface(Optional<Gfx::SystemUIFontKind> system_ui_font_kind, String family_name, u16 weight, u16 width, u8 slope)
{
    if (system_ui_font_kind.has_value()) {
        auto typeface = MUST(Gfx::TypefaceSkia::match_system_ui(system_ui_font_kind.value(), 0, weight, width, slope));
        if (typeface)
            return typeface.release_nonnull();
    }

    auto typeface = MUST(Gfx::TypefaceSkia::match_family_style(family_name.bytes_as_string_view(), weight, width, slope));
    VERIFY(typeface);
    return typeface.release_nonnull();
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::Typeface const& typeface)
{
    typeface.encode_font_data_for_ipc(encoder);
    return {};
}

template<>
ErrorOr<NonnullRefPtr<Gfx::Typeface const>> decode(Decoder& decoder)
{
    auto format = TRY(decoder.decode<Gfx::Typeface::FontDataFormat>());

    switch (format) {
    case Gfx::Typeface::FontDataFormat::RawFontData: {
        auto font_data = TRY(decoder.decode<Core::AnonymousBuffer>());
        auto ttc_index = TRY(decoder.decode<u32>());
        if (!font_data.is_valid())
            return Error::from_string_literal("Typeface IPC data contained invalid font data");
        return TRY(Gfx::Typeface::try_load_from_anonymous_buffer(move(font_data), ttc_index));
    }
    case Gfx::Typeface::FontDataFormat::ResourceFontData: {
        auto resource_uri = TRY(decoder.decode<String>());
        auto ttc_index = TRY(decoder.decode<u32>());
        auto resource = TRY(Core::Resource::load_from_uri(resource_uri.bytes_as_string_view()));
        return TRY(Gfx::Typeface::try_load_from_resource(*resource, ttc_index));
    }
    case Gfx::Typeface::FontDataFormat::SystemFont: {
        auto system_ui_font_kind = TRY(decoder.decode<Optional<Gfx::SystemUIFontKind>>());
        auto family_name = TRY(decoder.decode<String>());
        auto weight = TRY(decoder.decode<u16>());
        auto width = TRY(decoder.decode<u16>());
        auto slope = TRY(decoder.decode<u8>());
        return match_system_typeface(system_ui_font_kind, move(family_name), weight, width, slope);
    }
    }

    return Error::from_string_literal("Typeface IPC data contained invalid font data format");
}

}
