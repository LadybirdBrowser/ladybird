/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/SharedFontProvider.h>
#include <LibGfx/Font/WOFF/Loader.h>

namespace Gfx {

ErrorOr<NonnullOwnPtr<SharedFontProvider>> SharedFontProvider::create(NonnullOwnPtr<Core::MappedFile> mapping, u64 generation, SharedFontProviderCallbacks&& callbacks)
{
    auto catalog = TRY(FontCatalog::parse(mapping->bytes(), generation));
    return adopt_nonnull_own_or_enomem(new (nothrow) SharedFontProvider(move(mapping), move(catalog), move(callbacks)));
}

ErrorOr<NonnullOwnPtr<SharedFontProvider>> SharedFontProvider::create_from_catalog_file_or_empty(IPC::File file, u64 size, u64 generation, SharedFontProviderCallbacks&& callbacks)
{
    if (size == 0 || size > NumericLimits<size_t>::max()) {
        dbgln("SharedFontProvider: Refusing invalid font catalog size {}. Using an empty catalog.", size);
        return create_empty(generation, move(callbacks));
    }

    auto mapping = Core::MappedFile::map_from_fd_range_and_close(file.take_fd(), "font catalog"sv, 0, static_cast<size_t>(size));
    if (mapping.is_error()) {
        dbgln("SharedFontProvider: Unable to map font catalog: {}. Using an empty catalog.", mapping.error());
        return create_empty(generation, move(callbacks));
    }

    auto provider = create(mapping.release_value(), generation, move(callbacks));
    if (provider.is_error()) {
        dbgln("SharedFontProvider: Unable to parse font catalog: {}. Using an empty catalog.", provider.error());
        return create_empty(generation, move(callbacks));
    }
    return provider;
}

ErrorOr<NonnullOwnPtr<SharedFontProvider>> SharedFontProvider::create_empty(u64 generation, SharedFontProviderCallbacks&& callbacks)
{
    auto builder = TRY(FontCatalogBuilder::create(generation));
    auto serialized = TRY(builder->serialize());
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(serialized.size()));
    serialized.bytes().copy_to({ buffer.data<u8>(), buffer.size() });
    auto file = TRY(IPC::File::clone_fd(buffer.fd()));
    auto mapping = TRY(Core::MappedFile::map_from_fd_range_and_close(file.take_fd(), "empty font catalog"sv, 0, serialized.size()));
    return create(move(mapping), generation, move(callbacks));
}

SharedFontProvider::SharedFontProvider(NonnullOwnPtr<Core::MappedFile> mapping, NonnullOwnPtr<FontCatalog> catalog, SharedFontProviderCallbacks callbacks)
    : m_catalog_mapping(move(mapping))
    , m_catalog(move(catalog))
    , m_callbacks(move(callbacks))
{
    m_resource_fonts.load_all_fonts_from_uri("resource://fonts"sv);
}

SharedFontProvider::~SharedFontProvider()
{
    clear_typeface_cache();
}

void SharedFontProvider::clear_typeface_cache()
{
    for (auto const& entry : m_typeface_cache)
        entry.value->clear_font_cache();
    m_typeface_cache.clear();
}

ErrorOr<void> SharedFontProvider::replace_catalog(NonnullOwnPtr<Core::MappedFile> mapping, u64 generation)
{
    auto catalog = TRY(FontCatalog::parse(mapping->bytes(), generation));
    m_catalog = move(catalog);
    m_catalog_mapping = move(mapping);
    clear_typeface_cache();
    m_failed_face_ids.clear();
    m_code_point_cache.clear();
    return {};
}

ErrorOr<void> SharedFontProvider::replace_catalog(IPC::File file, u64 size, u64 generation)
{
    if (size == 0 || size > NumericLimits<size_t>::max())
        return Error::from_string_literal("Invalid font catalog size");
    auto mapping = TRY(Core::MappedFile::map_from_fd_range_and_close(file.take_fd(), "font catalog"sv, 0, static_cast<size_t>(size)));
    return replace_catalog(move(mapping), generation);
}

static FontVariationSettings default_variations(float point_size, unsigned weight, unsigned width)
{
    FontVariationSettings settings;
    settings.set_weight(static_cast<float>(weight));
    settings.set_optical_sizing(point_size / 0.75f);

    switch (width) {
    case FontWidth::UltraCondensed:
        settings.set_width(50);
        break;
    case FontWidth::ExtraCondensed:
        settings.set_width(62.5);
        break;
    case FontWidth::Condensed:
        settings.set_width(75);
        break;
    case FontWidth::SemiCondensed:
        settings.set_width(87.5);
        break;
    case FontWidth::Normal:
        settings.set_width(100);
        break;
    case FontWidth::SemiExpanded:
        settings.set_width(112.5);
        break;
    case FontWidth::Expanded:
        settings.set_width(125);
        break;
    case FontWidth::ExtraExpanded:
        settings.set_width(150);
        break;
    case FontWidth::UltraExpanded:
        settings.set_width(200);
        break;
    default:
        settings.set_width(100);
        break;
    }

    return settings;
}

static ShapeFeatures default_shape_features()
{
    return {
        { { 'c', 'l', 'i', 'g' }, 1 },
        { { 'k', 'e', 'r', 'n' }, 1 },
        { { 'l', 'i', 'g', 'a' }, 1 },
    };
}

RefPtr<Gfx::Font> SharedFontProvider::get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope, Optional<FontVariationSettings> const& variation_settings, Optional<Gfx::ShapeFeatures> const& shape_features)
{
    if (auto resource_font = m_resource_fonts.get_font(family, point_size, weight, width, slope, variation_settings, shape_features))
        return resource_font;

    RefPtr<Typeface> typeface;
    if (auto face = m_catalog->match_style(family.bytes_as_string_view(), weight, width, slope); face.has_value()) {
        typeface = load_catalog_face(*face);
    } else if (m_callbacks.match_font) {
        auto family_string = family.to_string();
        typeface = load_brokered_font(m_callbacks.match_font(family_string, weight, width, slope));
    }
    if (!typeface)
        return nullptr;

    return typeface->font(point_size, variation_settings.value_or_lazy_evaluated([&] { return default_variations(point_size, weight, width); }), shape_features.value_or_lazy_evaluated(default_shape_features));
}

void SharedFontProvider::for_each_typeface_with_family_name(FlyString const& family, Function<void(Typeface const&)> callback)
{
    m_resource_fonts.for_each_typeface_with_family_name(family, [&](Typeface const& typeface) {
        callback(typeface);
    });

    auto family_name = family.bytes_as_string_view();
    auto count = m_catalog->family_face_count(family_name);
    for (size_t index = 0; index < count; ++index) {
        auto face = m_catalog->family_face_at(family_name, index);
        if (!face.has_value())
            continue;
        if (auto typeface = load_catalog_face(*face))
            callback(*typeface);
    }
}

RefPtr<Typeface> SharedFontProvider::get_typeface_by_id(u64 generation, u64 face_id)
{
    if (generation != m_catalog->generation() || face_id == 0)
        return nullptr;
    if (auto typeface = m_typeface_cache.get(face_id); typeface.has_value())
        return *typeface;
    if (auto face = m_catalog->face_by_id(face_id); face.has_value())
        return load_catalog_face(*face);
    if (!m_callbacks.open_font)
        return nullptr;
    return load_brokered_font(m_callbacks.open_font(generation, face_id));
}

RefPtr<Gfx::Font> SharedFontProvider::get_font_for_code_point(u32 code_point, float point_size, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    if (!m_callbacks.match_font_for_code_point)
        return nullptr;

    CodePointCacheKey key { code_point, weight, width, slope, prefer_color_emoji };
    if (auto cached_typeface = m_code_point_cache.get(key); cached_typeface.has_value()) {
        if (!cached_typeface.value())
            return nullptr;
        return cached_typeface.value()->font(point_size, {});
    }

    auto typeface = load_brokered_font(m_callbacks.match_font_for_code_point(code_point, weight, width, slope, prefer_color_emoji));
    m_code_point_cache.set(key, typeface);
    if (!typeface)
        return nullptr;
    return typeface->font(point_size, {});
}

Optional<FlyString> SharedFontProvider::resolve_generic_family(StringView family_name, u16 weight, u8 slope)
{
    if (!m_callbacks.resolve_generic_family)
        return {};
    auto family = String::from_utf8(family_name);
    if (family.is_error())
        return {};
    return m_callbacks.resolve_generic_family(family.release_value(), weight, slope);
}

RefPtr<Typeface> SharedFontProvider::load_catalog_face(FontCatalogFace const& face)
{
    if (auto cached = m_typeface_cache.get(face.face_id); cached.has_value())
        return *cached;
    if (m_failed_face_ids.contains(face.face_id) || !m_callbacks.open_font)
        return nullptr;
    auto brokered_font = m_callbacks.open_font(m_catalog->generation(), face.face_id);
    if (!brokered_font.file.has_value()) {
        m_failed_face_ids.set(face.face_id);
        return nullptr;
    }
    return load_font_file(face.face_id, face.ttc_index, face.format, brokered_font.file.release_value());
}

RefPtr<Typeface> SharedFontProvider::load_brokered_font(BrokeredFont brokered_font)
{
    if (brokered_font.face_id == 0 || !brokered_font.file.has_value())
        return nullptr;
    if (auto cached = m_typeface_cache.get(brokered_font.face_id); cached.has_value())
        return *cached;
    return load_font_file(brokered_font.face_id, brokered_font.ttc_index, brokered_font.format, brokered_font.file.release_value());
}

RefPtr<Typeface> SharedFontProvider::load_font_file(u64 face_id, u32 ttc_index, FontFileFormat format, IPC::File file)
{
    if (m_failed_face_ids.contains(face_id))
        return nullptr;

    auto mapped_file = Core::MappedFile::map_from_fd_and_close(file.take_fd(), "brokered font"sv);
    if (mapped_file.is_error()) {
        m_failed_face_ids.set(face_id);
        return nullptr;
    }

    ErrorOr<NonnullRefPtr<Typeface>> typeface_or_error = Error::from_string_literal("Unsupported font file format");
    if (format == FontFileFormat::WOFF)
        typeface_or_error = WOFF::try_load_from_bytes(mapped_file.value()->bytes(), ttc_index);
    else
        typeface_or_error = Typeface::try_load_from_mapped_file(mapped_file.release_value(), ttc_index);

    if (typeface_or_error.is_error()) {
        m_failed_face_ids.set(face_id);
        return nullptr;
    }

    auto typeface = typeface_or_error.release_value();
    typeface->set_system_font_identifier({ m_catalog->generation(), face_id });
    m_typeface_cache.set(face_id, typeface);
    return typeface;
}

}
