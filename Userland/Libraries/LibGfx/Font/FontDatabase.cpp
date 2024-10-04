/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/DeprecatedFlyString.h>
#include <AK/FlyString.h>
#include <AK/LexicalPath.h>
#include <AK/Queue.h>
#include <LibCore/Resource.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/ScaledFont.h>
#include <LibGfx/Font/WOFF/Loader.h>

namespace Gfx {

FontDatabase& FontDatabase::the()
{
    static FontDatabase s_the;
    return s_the;
}

struct FontDatabase::Private {
    bool force_fontconfig { false };
    HashMap<FlyString, Vector<NonnullRefPtr<Typeface>>, AK::ASCIICaseInsensitiveFlyStringTraits> typeface_by_family;
};

void FontDatabase::set_force_fontconfig(bool force_fontconfig)
{
    m_private->force_fontconfig = force_fontconfig;
}

bool FontDatabase::should_force_fontconfig() const
{
    return m_private->force_fontconfig;
}

void FontDatabase::load_all_fonts_from_uri(StringView uri)
{
    auto root_or_error = Core::Resource::load_from_uri(uri);
    if (root_or_error.is_error()) {
        if (root_or_error.error().is_errno() && root_or_error.error().code() == ENOENT) {
            return;
        }
        dbgln("FontDatabase::load_all_fonts_from_uri('{}'): {}", uri, root_or_error.error());
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
                auto& family = m_private->typeface_by_family.ensure(font->family(), [] {
                    return Vector<NonnullRefPtr<Typeface>> {};
                });
                family.append(font);
            }
        } else if (path.has_extension(".woff"sv)) {
            if (auto font_or_error = WOFF::try_load_from_resource(resource); !font_or_error.is_error()) {
                auto font = font_or_error.release_value();
                auto& family = m_private->typeface_by_family.ensure(font->family(), [] {
                    return Vector<NonnullRefPtr<Typeface>> {};
                });
                family.append(font);
            }
        }
        return IterationDecision::Continue;
    });
}

FontDatabase::FontDatabase()
    : m_private(make<Private>())
{
}

RefPtr<Gfx::Font> FontDatabase::get(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope)
{
    auto it = m_private->typeface_by_family.find(family);
    if (it == m_private->typeface_by_family.end())
        return nullptr;
    for (auto const& typeface : it->value) {
        if (typeface->weight() == weight && typeface->width() == width && typeface->slope() == slope)
            return typeface->scaled_font(point_size);
    }
    return nullptr;
}

void FontDatabase::for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)> callback)
{
    auto it = m_private->typeface_by_family.find(family_name);
    if (it == m_private->typeface_by_family.end())
        return;
    for (auto const& typeface : it->value)
        callback(*typeface);
}

}
