/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <AK/HashMap.h>
#include <LibGfx/Resource/FontResource.h>
#include <LibPaintServer/Debug.h>
#include <PaintServer/FontResourceCache.h>
#include <core/SkData.h>
#include <core/SkFontArguments.h>
#include <core/SkFontMgr.h>
#include <core/SkStream.h>
#include <core/SkTypeface.h>

#if defined(AK_OS_MACOS)
#    include <ports/SkFontMgr_mac_ct.h>
#endif
#if defined(AK_OS_WINDOWS)
#    include <ports/SkTypeface_win.h>
#else
#    include <ports/SkFontMgr_fontconfig.h>
#    include <ports/SkFontScanner_FreeType.h>
#endif

namespace PaintServer {

static StringView font_data_kind(ReadonlyBytes bytes)
{
    if (bytes.size() < 4)
        return "short"sv;
    if (__builtin_memcmp(bytes.data(), "ttcf", 4) == 0)
        return "ttcf"sv;
    if (__builtin_memcmp(bytes.data(), "OTTO", 4) == 0)
        return "OTTO"sv;
    if (__builtin_memcmp(bytes.data(), "true", 4) == 0)
        return "sfnt"sv;
    if (bytes[0] == 0 && bytes[1] == 1 && bytes[2] == 0 && bytes[3] == 0)
        return "sfnt"sv;
    return "other"sv;
}

static Optional<u32> ttc_font_count(ReadonlyBytes bytes)
{
    if (bytes.size() < 12)
        return {};
    if (__builtin_memcmp(bytes.data(), "ttcf", 4) != 0)
        return {};

    u32 count = 0;
    __builtin_memcpy(&count, bytes.data() + 8, sizeof(count));
    return AK::convert_between_host_and_big_endian(count);
}

static ByteString first_four_bytes_hex(ReadonlyBytes bytes)
{
    u8 byte0 = bytes.size() > 0 ? bytes[0] : 0;
    u8 byte1 = bytes.size() > 1 ? bytes[1] : 0;
    u8 byte2 = bytes.size() > 2 ? bytes[2] : 0;
    u8 byte3 = bytes.size() > 3 ? bytes[3] : 0;
    return ByteString::formatted("{:02x}{:02x}{:02x}{:02x}", byte0, byte1, byte2, byte3);
}

static SkFontStyle::Slant slope_to_skia_slant(u8 slope)
{
    switch (slope) {
    case 1:
        return SkFontStyle::kItalic_Slant;
    case 2:
        return SkFontStyle::kOblique_Slant;
    default:
        return SkFontStyle::kUpright_Slant;
    }
}

static sk_sp<SkFontMgr> s_font_manager;
static bool s_force_fontconfig = false;

void set_force_fontconfig(bool force_fontconfig)
{
    VERIFY(!s_font_manager);
    s_force_fontconfig = force_fontconfig;
}

static SkFontMgr& font_manager()
{
    if (!s_font_manager) {
#if defined(AK_OS_MACOS)
        if (!s_force_fontconfig)
            s_font_manager = SkFontMgr_New_CoreText(nullptr);
#endif
#if defined(AK_OS_ANDROID)
        s_font_manager = SkFontMgr_New_Android(nullptr);
#elif defined(AK_OS_WINDOWS)
        s_font_manager = SkFontMgr_New_DirectWrite();
#else
        if (!s_font_manager)
            s_font_manager = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
    }
    return *s_font_manager;
}

struct FontResourceCache::Impl {
    HashMap<SurfaceID, HashMap<ResourceID, sk_sp<SkTypeface>>> typefaces_by_surface;
};

static ErrorOr<sk_sp<SkTypeface>> clone_typeface_with_variations(sk_sp<SkTypeface> typeface, ReadonlySpan<Gfx::FontVariationAxis const> variation_axes)
{
    if (variation_axes.is_empty())
        return typeface;

    SkFontArguments font_args;
    Vector<SkFontArguments::VariationPosition::Coordinate> coords;
    coords.ensure_capacity(variation_axes.size());
    for (auto const& axis : variation_axes)
        coords.unchecked_append({ axis.tag.to_u32(), axis.value });

    SkFontArguments::VariationPosition variation_position;
    variation_position.coordinates = coords.data();
    variation_position.coordinateCount = static_cast<int>(coords.size());
    font_args.setVariationDesignPosition(variation_position);

    auto cloned_typeface = typeface->makeClone(font_args);
    if (!cloned_typeface)
        return Error::from_string_literal("Failed to apply font variations");

    return cloned_typeface;
}

FontResourceCache::FontResourceCache()
    : m_impl(make<Impl>())
{
}

FontResourceCache::FontResourceCache(FontResourceCache&&) = default;
FontResourceCache& FontResourceCache::operator=(FontResourceCache&&) = default;
FontResourceCache::~FontResourceCache() = default;

ErrorOr<void> FontResourceCache::register_font(SurfaceID surface_id, ResourceID font_resource_id, ReadonlyBytes typeface_bytes, u32 ttc_index)
{
    if (auto surface_map = m_impl->typefaces_by_surface.get(surface_id); surface_map.has_value()) {
        surface_map->remove(font_resource_id);
        if (surface_map->is_empty())
            m_impl->typefaces_by_surface.remove(surface_id);
    }

    ReadonlySpan<Gfx::FontVariationAxis const> variation_axes;
    Vector<Gfx::FontVariationAxis> decoded_variation_axes;
    if (Gfx::has_skia_typeface_payload_header(typeface_bytes)) {
        auto decoded_payload = TRY(Gfx::bytes_to_skia_typeface_payload(typeface_bytes));
        ttc_index = decoded_payload.ttc_index;
        decoded_variation_axes = move(decoded_payload.variation_axes);
        variation_axes = decoded_variation_axes.span();
        typeface_bytes = decoded_payload.typeface_bytes;
    }

    auto sk_data = SkData::MakeWithCopy(typeface_bytes.data(), typeface_bytes.size());
    if (!sk_data)
        return Error::from_string_literal("Failed to copy typeface data for SkTypeface");

    SkFontArguments font_args;
    font_args.setCollectionIndex(static_cast<int>(ttc_index));

    auto stream = std::make_unique<SkMemoryStream>(sk_data);
    auto typeface = font_manager().makeFromStream(std::move(stream), font_args);
    if (!typeface) {
        if (is_logging_enabled(LOG_RESOURCE)) {
            Optional<u32> collection_font_count = ttc_font_count(typeface_bytes);
            dbgln("FontResourceCache: register_font failed {} surface_id={} size={} ttc_index={} kind={} first4={} ttc_font_count={}",
                font_resource_id,
                surface_id,
                typeface_bytes.size(),
                ttc_index,
                font_data_kind(typeface_bytes),
                first_four_bytes_hex(typeface_bytes),
                collection_font_count.value_or(0));
        }
        return Error::from_string_literal("Failed to create SkTypeface from font data");
    }

    typeface = TRY(clone_typeface_with_variations(move(typeface), variation_axes));

    auto& surface_map = m_impl->typefaces_by_surface.ensure(surface_id, [] { return HashMap<ResourceID, sk_sp<SkTypeface>> {}; });
    surface_map.set(font_resource_id, move(typeface));
    return {};
}

ErrorOr<void> FontResourceCache::register_local_font(SurfaceID surface_id, ResourceID font_resource_id, ReadonlyBytes payload_bytes)
{
    if (auto surface_map = m_impl->typefaces_by_surface.get(surface_id); surface_map.has_value()) {
        surface_map->remove(font_resource_id);
        if (surface_map->is_empty())
            m_impl->typefaces_by_surface.remove(surface_id);
    }

    auto local_font_info = TRY(Gfx::bytes_to_local_font_info(payload_bytes));

    SkFontStyle const style(local_font_info.weight, local_font_info.width, slope_to_skia_slant(local_font_info.slope));

    sk_sp<SkTypeface> typeface;
    if (!local_font_info.resource_name.is_empty())
        typeface = font_manager().makeFromFile(local_font_info.resource_name.characters(), static_cast<int>(local_font_info.ttc_index));
    if (!typeface)
        typeface = font_manager().matchFamilyStyle(local_font_info.family_name.is_empty() ? nullptr : local_font_info.family_name.characters(), style);
    if (!typeface) {
        if (is_logging_enabled(LOG_RESOURCE)) {
            dbgln("FontResourceCache: register_local_font failed {} surface_id={} payload_size={} resource_name={} family={} weight={} width={} slope={} ttc_index={} variation_axis_count={} error=no_matching_typeface",
                font_resource_id,
                surface_id,
                payload_bytes.size(),
                local_font_info.resource_name,
                local_font_info.family_name,
                local_font_info.weight,
                local_font_info.width,
                local_font_info.slope,
                local_font_info.ttc_index,
                local_font_info.variation_axes.size());
        }
        return Error::from_string_literal("Failed to resolve local font typeface");
    }

    typeface = TRY(clone_typeface_with_variations(move(typeface), local_font_info.variation_axes.span()));

    auto& surface_map = m_impl->typefaces_by_surface.ensure(surface_id, [] { return HashMap<ResourceID, sk_sp<SkTypeface>> {}; });
    surface_map.set(font_resource_id, move(typeface));

    if (is_logging_enabled(LOG_RESOURCE)) {
        dbgln("FontResourceCache: registered local font {} surface_id={} payload_size={} resource_name={} family={} weight={} width={} slope={} ttc_index={} variation_axis_count={}",
            font_resource_id,
            surface_id,
            payload_bytes.size(),
            local_font_info.resource_name,
            local_font_info.family_name,
            local_font_info.weight,
            local_font_info.width,
            local_font_info.slope,
            local_font_info.ttc_index,
            local_font_info.variation_axes.size());
    }

    return {};
}

void FontResourceCache::unregister_font(SurfaceID surface_id, ResourceID font_resource_id)
{
    auto surface_it = m_impl->typefaces_by_surface.find(surface_id);
    if (surface_it == m_impl->typefaces_by_surface.end())
        return;
    surface_it->value.remove(font_resource_id);
    if (surface_it->value.is_empty())
        m_impl->typefaces_by_surface.remove(surface_it);
}

void FontResourceCache::clear_surface(SurfaceID surface_id)
{
    m_impl->typefaces_by_surface.remove(surface_id);
}

SkTypeface* FontResourceCache::lookup(SurfaceID surface_id, ResourceID font_resource_id) const
{
    auto surface_it = m_impl->typefaces_by_surface.find(surface_id);
    if (surface_it == m_impl->typefaces_by_surface.end())
        return nullptr;

    auto font_it = surface_it->value.find(font_resource_id);
    if (font_it == surface_it->value.end())
        return nullptr;
    return font_it->value.get();
}

void FontResourceCache::clear()
{
    m_impl->typefaces_by_surface.clear();
}

}
