/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/DeprecatedFlyString.h>
#include <AK/FlyString.h>
#include <AK/LexicalPath.h>
#include <AK/Queue.h>
#include <AK/QuickSort.h>
#include <LibCore/Resource.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/OpenType/Font.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/WOFF/Font.h>

namespace Gfx {

FontDatabase& FontDatabase::the()
{
    static FontDatabase s_the;
    return s_the;
}

struct FontDatabase::Private {
    HashMap<ByteString, NonnullRefPtr<Gfx::Font>, CaseInsensitiveStringTraits> full_name_to_font_map;
    HashMap<FlyString, Vector<NonnullRefPtr<Typeface>>, AK::ASCIICaseInsensitiveFlyStringTraits> typefaces;
};

void FontDatabase::load_all_fonts_from_uri(StringView uri)
{
    auto root_or_error = Core::Resource::load_from_uri(uri);
    if (root_or_error.is_error()) {
        dbgln("FontDatabase::load_all_fonts_from_uri('{}'): {}", uri, root_or_error.error());
        return;
    }
    auto root = root_or_error.release_value();

    root->for_each_descendant_file([this](Core::Resource const& resource) -> IterationDecision {
        auto uri = resource.uri();
        auto path = LexicalPath(uri.bytes_as_string_view());
        if (path.has_extension(".ttf"sv)) {
            // FIXME: What about .otf
            if (auto font_or_error = OpenType::Font::try_load_from_resource(resource); !font_or_error.is_error()) {
                auto font = font_or_error.release_value();
                auto typeface = get_or_create_typeface(font->family(), font->variant());
                typeface->set_vector_font(move(font));
            }
        } else if (path.has_extension(".woff"sv)) {
            if (auto font_or_error = WOFF::Font::try_load_from_resource(resource); !font_or_error.is_error()) {
                auto font = font_or_error.release_value();
                auto typeface = get_or_create_typeface(font->family(), font->variant());
                typeface->set_vector_font(move(font));
            }
        }
        return IterationDecision::Continue;
    });
}

FontDatabase::FontDatabase()
    : m_private(make<Private>())
{
    load_all_fonts_from_uri("resource://fonts"sv);
}

void FontDatabase::for_each_font(Function<void(Gfx::Font const&)> callback)
{
    Vector<RefPtr<Gfx::Font>> fonts;
    fonts.ensure_capacity(m_private->full_name_to_font_map.size());
    for (auto& it : m_private->full_name_to_font_map)
        fonts.append(it.value);
    quick_sort(fonts, [](auto& a, auto& b) { return a->qualified_name() < b->qualified_name(); });
    for (auto& font : fonts)
        callback(*font);
}

RefPtr<Gfx::Font> FontDatabase::get_by_name(StringView name)
{
    auto it = m_private->full_name_to_font_map.find(name);
    if (it == m_private->full_name_to_font_map.end()) {
        auto parts = name.split_view(" "sv);
        if (parts.size() >= 4) {
            auto slope = parts.take_last().to_number<int>().value_or(0);
            auto weight = parts.take_last().to_number<int>().value_or(0);
            auto size = parts.take_last().to_number<int>().value_or(0);
            auto family = MUST(String::join(' ', parts));
            return get(family, size, weight, Gfx::FontWidth::Normal, slope);
        }
        dbgln("Font lookup failed: '{}'", name);
        return nullptr;
    }
    return it->value;
}

RefPtr<Gfx::Font> FontDatabase::get(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope)
{
    auto it = m_private->typefaces.find(family);
    if (it == m_private->typefaces.end())
        return nullptr;
    for (auto const& typeface : it->value) {
        if (typeface->weight() == weight && typeface->width() == width && typeface->slope() == slope)
            return typeface->get_font(point_size);
    }
    return nullptr;
}

RefPtr<Gfx::Font> FontDatabase::get(FlyString const& family, FlyString const& variant, float point_size)
{
    auto it = m_private->typefaces.find(family);
    if (it == m_private->typefaces.end())
        return nullptr;
    for (auto const& typeface : it->value) {
        if (typeface->variant() == variant)
            return typeface->get_font(point_size);
    }
    return nullptr;
}

RefPtr<Typeface> FontDatabase::get_or_create_typeface(FlyString const& family, FlyString const& variant)
{
    auto it = m_private->typefaces.find(family);
    if (it != m_private->typefaces.end()) {
        for (auto const& typeface : it->value) {
            if (typeface->variant() == variant)
                return typeface;
        }
    }
    auto typeface = adopt_ref(*new Typeface(family, variant));
    m_private->typefaces.ensure(family).append(typeface);
    return typeface;
}

void FontDatabase::for_each_typeface(Function<void(Typeface const&)> callback)
{
    for (auto const& it : m_private->typefaces) {
        for (auto const& jt : it.value) {
            callback(*jt);
        }
    }
}

void FontDatabase::for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)> callback)
{
    auto it = m_private->typefaces.find(family_name);
    if (it == m_private->typefaces.end())
        return;
    for (auto const& typeface : it->value)
        callback(*typeface);
}

}
