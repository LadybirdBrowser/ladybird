/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef USE_FONTCONFIG
#    error "FontconfigFontProvider requires USE_FONTCONFIG to be enabled"
#endif

#include <AK/RefPtr.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontconfigFontProvider.h>
#include <LibGfx/Font/ScaledFont.h>
#include <LibGfx/Font/WOFF/Loader.h>

#include <fontconfig/fontconfig.h>

namespace Gfx {

FontconfigFontProvider::FontconfigFontProvider()
{
    [[maybe_unused]] auto fontconfig_initialized = FcInit();
    VERIFY(fontconfig_initialized);
}

FontconfigFontProvider::~FontconfigFontProvider() = default;

void FontconfigFontProvider::add_uri_to_config(StringView uri)
{
    auto* config = FcConfigGetCurrent();
    VERIFY(config);

    auto path = MUST(Core::Resource::load_from_uri(uri));
    VERIFY(path->is_directory());
    ByteString const fs_path = path->filesystem_path().to_byte_string();

    bool const success = FcConfigAppFontAddDir(config, reinterpret_cast<FcChar8 const*>(fs_path.characters()));
    VERIFY(success);
}

RefPtr<Typeface> FontconfigFontProvider::load_typeface_from_path(ByteString const& path, int index)
{
    dbgln_if(FONTCONFIG_DEBUG, "FontconfigFontProvider: Loading font {} from {}", index, path);

    auto resource = Core::Resource::load_from_filesystem(path);
    if (resource.is_error())
        return nullptr;
    auto typeface_or_error = Typeface::try_load_from_resource(resource.release_value(), index);
    if (typeface_or_error.is_error()) {
        typeface_or_error = WOFF::try_load_from_resource(resource.release_value(), index);
        if (typeface_or_error.is_error())
            return nullptr;
    }
    auto typeface = typeface_or_error.release_value();

    auto& family_list = m_typeface_by_family.ensure(typeface->family(), [] {
        return Vector<NonnullRefPtr<Typeface>> {};
    });
    family_list.append(typeface);
    return typeface;
}

RefPtr<Gfx::Font> FontconfigFontProvider::get_font(FlyString const& family, float point_size, unsigned int weight, unsigned int width, unsigned int slope)
{
    if (auto typefaces = m_typeface_by_family.get(family); typefaces.has_value()) {
        for (auto& typeface : *typefaces) {
            if (typeface->weight() == weight && typeface->width() == width && typeface->slope() == slope)
                return typeface->scaled_font(point_size);
        }
    }

    // FIXME: We should be able to avoid this allocation with a null-terminated flystring
    ByteString const nullterm_family = family.bytes_as_string_view();

    auto* config = FcConfigGetCurrent();
    VERIFY(config);

    auto* pattern = FcPatternBuild(nullptr, FC_FAMILY, FcTypeString, reinterpret_cast<FcChar8 const*>(nullterm_family.characters()), nullptr);
    VERIFY(pattern);
    ScopeGuard const pattern_guard { [pattern] { FcPatternDestroy(pattern); } };

    auto success = FcConfigSubstitute(config, pattern, FcMatchPattern);
    VERIFY(success);

    FcDefaultSubstitute(pattern);

    FcResult result {};
    auto* matched_pattern = FcFontMatch(config, pattern, &result);
    if (result != FcResultMatch || !matched_pattern)
        return nullptr;
    ScopeGuard const matched_pattern_guard { [matched_pattern] { FcPatternDestroy(matched_pattern); } };

    FcChar8* file = nullptr;
    if (FcPatternGetString(matched_pattern, FC_FILE, 0, &file) != FcResultMatch)
        return nullptr;
    auto filename = ByteString { reinterpret_cast<char const*>(file) };

    int index = 0;
    if (FcPatternGetInteger(matched_pattern, FC_INDEX, 0, &index) != FcResultMatch)
        return nullptr;

    auto typeface = load_typeface_from_path(filename, index);
    return typeface ? typeface->scaled_font(point_size) : nullptr;
}

Optional<FontDescription> FontconfigFontProvider::description_for_fontconfig_parameters(FlyString const& family, ByteString const& path, int index, int weight, int width, int slant)
{
    // FIXME: Do better validation and normalization of fontconfig parameters
    if (weight < 0 || weight > 1000) {
        dbgln_if(FONTCONFIG_DEBUG, "FontconfigFontProvider: Invalid weight {} for font {} in {}@{}", weight, family, path, index);
        return {};
    }
    if (width < 0 || width > 9) {
        dbgln_if(FONTCONFIG_DEBUG, "FontconfigFontProvider: Invalid width {} for font {} in {}@{}", width, family, path, index);
        return {};
    }
    FontSlant normalized_slant = FontSlant::Upright;
    if (slant == FC_SLANT_ITALIC)
        normalized_slant = FontSlant::Italic;
    else if (slant == FC_SLANT_OBLIQUE)
        normalized_slant = FontSlant::Oblique;
    else if (slant != FC_SLANT_ROMAN) {
        dbgln_if(FONTCONFIG_DEBUG, "FontconfigFontProvider: Invalid slant {} for font {} in {}@{}", slant, family, path, index);
        return {};
    }

    return FontDescription {
        .family = family,
        .weight = static_cast<u16>(weight),
        .width = static_cast<FontWidth>(width),
        .slant = normalized_slant,
        .load_typeface = [this, family, path, index, weight, width, slant]() -> RefPtr<Typeface> {
            // FIXME: Use more normalized values here to check cache
            if (auto typefaces = m_typeface_by_family.get(family); typefaces.has_value()) {
                for (auto& typeface : *typefaces) {
                    if (typeface->weight() == weight && typeface->width() == width && typeface->slope() == slant)
                        return typeface;
                }
            }
            return load_typeface_from_path(path, index);
        },
    };
}

void FontconfigFontProvider::for_each_typeface_with_family_name(FlyString const& family_name, Function<void(FontDescription)> callback)
{
    auto* config = FcConfigGetCurrent();
    VERIFY(config);

    auto* set = FcConfigGetFonts(config, FcSetSystem);
    VERIFY(set);

    // FIXME: We should be able to avoid this allocation with a null-terminated flystring
    ByteString const nullterm_family = family_name.bytes_as_string_view();

    auto* pattern = FcPatternBuild(nullptr, FC_FAMILY, FcTypeString, reinterpret_cast<FcChar8 const*>(nullterm_family.characters()), nullptr);
    VERIFY(pattern);
    auto pattern_guard = ScopeGuard { [pattern] { FcPatternDestroy(pattern); } };

    auto* object_set = FcObjectSetBuild(FC_FAMILY, FC_WEIGHT, FC_WIDTH, FC_SLANT, FC_FILE, FC_INDEX, nullptr);
    VERIFY(object_set);
    auto object_set_guard = ScopeGuard { [object_set] { FcObjectSetDestroy(object_set); } };

    auto* matches = FcFontSetList(config, &set, 1, pattern, object_set);
    if (!matches)
        return;
    ScopeGuard const matches_guard { [matches] { FcFontSetDestroy(matches); } };

    FcResult result {};
    for (auto idx = 0; idx < matches->nfont; ++idx) {
        auto* current_pattern = matches->fonts[idx];

        FcChar8* path = nullptr;
        result = FcPatternGetString(current_pattern, FC_FILE, 0, &path);
        VERIFY(result == FcResultMatch);
        auto pattern_path = ByteString { reinterpret_cast<char const*>(path) };

        int pattern_index = 0;
        result = FcPatternGetInteger(current_pattern, FC_INDEX, 0, &pattern_index);
        VERIFY(result == FcResultMatch);

        FcChar8* family = nullptr;
        result = FcPatternGetString(current_pattern, FC_FAMILY, 0, &family);
        VERIFY(result == FcResultMatch);
        StringView const family_view = StringView { reinterpret_cast<char const*>(family), strlen(reinterpret_cast<char const*>(family)) };
        auto pattern_family_or_error = FlyString::from_utf8(family_view);
        if (pattern_family_or_error.is_error()) {
            dbgln("FontconfigFontProvider: Failed to read UTF-8 family name for font {} in {}", pattern_index, pattern_path);
            continue;
        }
        auto pattern_family = pattern_family_or_error.release_value();

        int weight = 0;
        result = FcPatternGetInteger(current_pattern, FC_WEIGHT, 0, &weight);
        if (result != FcResultMatch) {
            dbgln_if(FONTCONFIG_DEBUG, "FontconfigFontProvider: Failed to read weight for font {} in {}@{}", pattern_family, pattern_path, pattern_index);
            continue;
        }

        int width = 0;
        result = FcPatternGetInteger(current_pattern, FC_WIDTH, 0, &width);
        if (result != FcResultMatch) {
            dbgln_if(FONTCONFIG_DEBUG, "FontconfigFontProvider: Failed to read width for font {} in {}@{}", pattern_family, pattern_path, pattern_index);
            continue;
        }

        int slant = 0;
        result = FcPatternGetInteger(current_pattern, FC_SLANT, 0, &slant);
        if (result != FcResultMatch) {
            dbgln_if(FONTCONFIG_DEBUG, "FontconfigFontProvider: Failed to read slant for font {} in {}@{}", pattern_family, pattern_path, pattern_index);
            continue;
        }

        if (auto descriptor = description_for_fontconfig_parameters(pattern_family, pattern_path, pattern_index, weight, width, slant); descriptor.has_value()) {
            callback(descriptor.release_value());
        }
    }
}

}
