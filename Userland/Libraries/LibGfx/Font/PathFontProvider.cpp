/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <LibCore/Resource.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/Font/ScaledFont.h>
#include <LibGfx/Font/WOFF/Loader.h>

namespace Gfx {

PathFontProvider::PathFontProvider() = default;
PathFontProvider::~PathFontProvider() = default;

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
        if (path.has_extension(".ttf"sv) || path.has_extension(".ttc"sv)) {
            // FIXME: What about .otf
            if (auto font_or_error = Typeface::try_load_from_resource(resource); !font_or_error.is_error()) {
                auto font = font_or_error.release_value();
                auto& family = m_typeface_by_family.ensure(font->family(), [] {
                    return Vector<NonnullRefPtr<Typeface>> {};
                });
                family.append(font);
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

RefPtr<Gfx::Font> PathFontProvider::get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope)
{
    auto it = m_typeface_by_family.find(family);
    if (it == m_typeface_by_family.end())
        return nullptr;
    for (auto const& typeface : it->value) {
        if (typeface->weight() == weight && typeface->width() == width && typeface->slope() == slope)
            return typeface->scaled_font(point_size);
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
