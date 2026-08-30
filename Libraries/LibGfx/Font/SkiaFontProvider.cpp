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
    auto make_font = [&](Typeface const& typeface) {
        return typeface.font(point_size,
            font_variation_settings.value_or_lazy_evaluated([&] { return default_font_variation_settings(point_size, weight, width); }),
            shape_features.value_or_lazy_evaluated([&] { return default_shape_features(); }));
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
    if (auto it = m_bundled_typefaces.find(family_name); it != m_bundled_typefaces.end()) {
        for (auto const& typeface : it->value)
            callback(*typeface);
    }

    auto system_typefaces = ensure_system_family_cached(family_name);
    for (auto const& typeface : system_typefaces)
        callback(*typeface);
}

}
