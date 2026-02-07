/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/Endian.h>
#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <LibCore/Resource.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/Font/WOFF/Loader.h>

namespace Gfx {

PathFontProvider::PathFontProvider() = default;
PathFontProvider::~PathFontProvider() = default;

// https://learn.microsoft.com/en-us/typography/opentype/spec/otff#ttc-header
static u32 number_of_fonts_in_ttc(ReadonlyBytes bytes)
{
    // TTC Header:
    //   0-3: ttcTag ('ttcf')
    //   4-7: majorVersion, minorVersion
    //   8-11: numFonts (big-endian u32)
    if (bytes.size() < 12)
        return 1;
    if (bytes.slice(0, 4) != "ttcf"sv.bytes())
        return 1;
    return AK::convert_between_host_and_big_endian(ByteReader::load32(bytes.offset(8)));
}

void PathFontProvider::load_all_fonts_from_uri(StringView uri)
{
    auto root_or_error = Core::Resource::load_from_uri(uri);
    if (root_or_error.is_error()) {
        if (root_or_error.error().is_errno() && root_or_error.error().code() == ENOENT) {
            return;
        }
        dbgln("PathFontProvider::load_all_fonts_from_uri('{}'): {}", uri, root_or_error.error());
        return;
    }
    auto root = root_or_error.release_value();

    root->for_each_descendant_file([this](Core::Resource const& resource) -> IterationDecision {
        auto uri = resource.uri();
        auto path = LexicalPath(uri.bytes_as_string_view());
        if (path.has_extension(".ttf"sv) || path.has_extension(".ttc"sv) || path.has_extension(".otf"sv)) {
            auto font_count = number_of_fonts_in_ttc(resource.data());
            for (u32 ttc_index = 0; ttc_index < font_count; ++ttc_index) {
                if (auto font_or_error = Typeface::try_load_from_resource(resource, ttc_index); !font_or_error.is_error()) {
                    auto font = font_or_error.release_value();
                    auto& family = m_typeface_by_family.ensure(font->family(), [] {
                        return Vector<NonnullRefPtr<Typeface>> {};
                    });
                    family.append(font);
                }
            }
        } else if (path.has_extension(".woff"sv)) {
            if (auto font_or_error = WOFF::try_load_from_resource(resource); !font_or_error.is_error()) {
                auto font = font_or_error.release_value();
                auto& family = m_typeface_by_family.ensure(font->family(), [] {
                    return Vector<NonnullRefPtr<Typeface>> {};
                });
                family.append(font);
            }
        }
        return IterationDecision::Continue;
    });
}

RefPtr<Gfx::Font> PathFontProvider::get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope, Optional<FontVariationSettings> const& font_variation_settings, Optional<Gfx::ShapeFeatures> const& shape_features)
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
        Gfx::ShapeFeatures shape_features;
        shape_features.append({ { 'c', 'l', 'i', 'g' }, 1 });
        shape_features.append({ { 'k', 'e', 'r', 'n' }, 1 });
        shape_features.append({ { 'l', 'i', 'g', 'a' }, 1 });
        return shape_features;
    };

    auto it = m_typeface_by_family.find(family);
    if (it == m_typeface_by_family.end())
        return nullptr;
    for (auto const& typeface : it->value) {
        if (typeface->weight() == weight && typeface->width() == width && typeface->slope() == slope)
            return typeface->font(point_size, font_variation_settings.value_or_lazy_evaluated([&] { return compute_default_font_variation_settings(weight, width); }), shape_features.value_or_lazy_evaluated([&] { return compute_default_shape_features(); }));
    }

    return nullptr;
}

void PathFontProvider::for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)> callback)
{
    auto it = m_typeface_by_family.find(family_name);
    if (it == m_typeface_by_family.end())
        return;
    for (auto const& typeface : it->value) {
        callback(*typeface);
    }
}

}
