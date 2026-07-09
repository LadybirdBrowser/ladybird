/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <LibCore/Resource.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/SkiaFontProvider.h>
#include <LibGfx/Font/WOFF/Loader.h>

namespace Gfx {

SkiaFontProvider::SkiaFontProvider() = default;
SkiaFontProvider::~SkiaFontProvider() = default;

void SkiaFontProvider::load_all_fonts_from_uri(StringView uri)
{
    auto root_or_error = Core::Resource::load_from_uri(uri);
    if (root_or_error.is_error()) {
        if (root_or_error.error().is_errno() && root_or_error.error().code() == ENOENT)
            return;
        dbgln("SkiaFontProvider::load_all_fonts_from_uri('{}'): {}", uri, root_or_error.error());
        return;
    }
    auto root = root_or_error.release_value();

    root->for_each_descendant_file([this](Core::Resource const& resource) -> IterationDecision {
        auto uri = resource.uri();
        auto path = LexicalPath(uri.bytes_as_string_view());
        RefPtr<Typeface> typeface;
        if (path.has_extension(".ttf"sv) || path.has_extension(".ttc"sv) || path.has_extension(".otf"sv)) {
            if (auto typeface_or_error = Typeface::try_load_from_resource(resource); !typeface_or_error.is_error())
                typeface = typeface_or_error.release_value();
        } else if (path.has_extension(".woff"sv)) {
            if (auto typeface_or_error = WOFF::try_load_from_resource(resource); !typeface_or_error.is_error())
                typeface = typeface_or_error.release_value();
        }
        if (typeface) {
            auto& family = m_bundled_typefaces.ensure(typeface->family(), [] {
                return Vector<NonnullRefPtr<Typeface>> {};
            });
            family.append(typeface.release_nonnull());
        }
        return IterationDecision::Continue;
    });
}

Vector<NonnullRefPtr<Typeface>> const& SkiaFontProvider::ensure_system_family_cached(FlyString const& family_name)
{
    return m_system_typefaces.ensure(family_name, [&] {
        Vector<NonnullRefPtr<Typeface>> typefaces;
        TypefaceSkia::for_each_typeface_in_family(family_name.bytes_as_string_view(), [&](NonnullRefPtr<TypefaceSkia> typeface) {
            // NB: Skia's fontconfig backend substitutes unknown families with a configured alias. Reject typefaces
            //     whose real family differs so the CSS fallback chain can try the next candidate instead of silently
            //     using the substitute.
            if (typeface->family().bytes_as_string_view().equals_ignoring_ascii_case(family_name.bytes_as_string_view()))
                typefaces.append(move(typeface));
        });
        return typefaces;
    });
}

RefPtr<Gfx::Font> SkiaFontProvider::get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope, Optional<FontVariationSettings> const& font_variation_settings, Optional<Gfx::ShapeFeatures> const& shape_features)
{
    auto const compute_default_font_variation_settings = [&](unsigned weight, unsigned width) {
        FontVariationSettings default_font_variation_settings;
        default_font_variation_settings.set_weight(static_cast<float>(weight));
        // NB: We use the pixel size for 'opsz'
        default_font_variation_settings.set_optical_sizing(point_size / 0.75f);

        switch (width) {
        case FontWidth::UltraCondensed:
            default_font_variation_settings.set_width(50);
            break;
        case FontWidth::ExtraCondensed:
            default_font_variation_settings.set_width(62.5);
            break;
        case FontWidth::Condensed:
            default_font_variation_settings.set_width(75);
            break;
        case FontWidth::SemiCondensed:
            default_font_variation_settings.set_width(87.5);
            break;
        case FontWidth::Normal:
            default_font_variation_settings.set_width(100);
            break;
        case FontWidth::SemiExpanded:
            default_font_variation_settings.set_width(112.5);
            break;
        case FontWidth::Expanded:
            default_font_variation_settings.set_width(125);
            break;
        case FontWidth::ExtraExpanded:
            default_font_variation_settings.set_width(150);
            break;
        case FontWidth::UltraExpanded:
            default_font_variation_settings.set_width(200);
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        return default_font_variation_settings;
    };

    auto const compute_default_shape_features = [&]() {
        // NB: These shape features match those applied when all CSS properties are initial values
        Gfx::ShapeFeatures default_shape_features;
        default_shape_features.append({ { 'c', 'l', 'i', 'g' }, 1 });
        default_shape_features.append({ { 'k', 'e', 'r', 'n' }, 1 });
        default_shape_features.append({ { 'l', 'i', 'g', 'a' }, 1 });
        return default_shape_features;
    };

    auto make_font = [&](Typeface const& typeface) {
        return typeface.font(point_size,
            font_variation_settings.value_or_lazy_evaluated([&] { return compute_default_font_variation_settings(weight, width); }),
            shape_features.value_or_lazy_evaluated([&] { return compute_default_shape_features(); }));
    };

    // Bundled fonts take priority over system fonts, and require an exact weight/width/slope match.
    if (auto it = m_bundled_typefaces.find(family); it != m_bundled_typefaces.end()) {
        for (auto const& typeface : it->value) {
            if (typeface->weight() == weight && typeface->width() == width && typeface->slope() == slope)
                return make_font(*typeface);
        }
    }

    for (auto const& typeface : ensure_system_family_cached(family)) {
        if (typeface->weight() == weight && typeface->width() == width && typeface->slope() == slope)
            return make_font(*typeface);
    }

    return nullptr;
}

void SkiaFontProvider::for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)> callback)
{
    // Bundled fonts shadow system fonts of the same family name.
    if (auto it = m_bundled_typefaces.find(family_name); it != m_bundled_typefaces.end()) {
        for (auto const& typeface : it->value)
            callback(*typeface);
        return;
    }

    for (auto const& typeface : ensure_system_family_cached(family_name))
        callback(*typeface);
}

}
