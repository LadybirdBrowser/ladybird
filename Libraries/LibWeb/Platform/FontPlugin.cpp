/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/TypeCasts.h>
#include <LibCore/Resource.h>
#include <LibCore/StandardPaths.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Platform {

static FontPlugin* s_the;

FontPlugin& FontPlugin::the()
{
    VERIFY(s_the);
    return *s_the;
}

void FontPlugin::install(FontPlugin& plugin)
{
    VERIFY(!s_the);
    s_the = &plugin;
}

FontPlugin::FontPlugin(bool is_layout_test_mode, Gfx::SystemFontProvider* font_provider)
    : m_is_layout_test_mode(is_layout_test_mode)
{
    if (!font_provider)
        font_provider = &static_cast<Gfx::PathFontProvider&>(Gfx::FontDatabase::the().install_system_font_provider(make<Gfx::PathFontProvider>()));
    if (is<Gfx::PathFontProvider>(*font_provider)) {
        auto& path_font_provider = static_cast<Gfx::PathFontProvider&>(*font_provider);
        // Load anything we can find in the system's font directories
        for (auto const& path : Gfx::FontDatabase::font_directories().release_value_but_fixme_should_propagate_errors())
            path_font_provider.load_all_fonts_from_uri(MUST(String::formatted("file://{}", path)));
    }

    update_generic_fonts();

    m_default_font_name = generic_font_name(GenericFont::UiSansSerif);

    auto default_fixed_width_font_name = generic_font_name(GenericFont::UiMonospace);
    m_default_fixed_width_font = Gfx::FontDatabase::the().get(default_fixed_width_font_name, 12.0, 400, Gfx::FontWidth::Normal, 0);
    VERIFY(m_default_fixed_width_font);

    if (is_layout_test_mode) {
        m_symbol_font_names = { "Noto Emoji"_fly_string };
    } else {
#ifdef AK_OS_MACOS
        m_symbol_font_names = { "Apple Color Emoji"_fly_string, "Apple Symbols"_fly_string };
#else
        m_symbol_font_names = { "Noto Color Emoji"_fly_string, "Noto Sans Symbols"_fly_string };
#endif
    }
}

FontPlugin::~FontPlugin() = default;

RefPtr<Gfx::Font> FontPlugin::default_font(float point_size, Optional<Gfx::FontVariationSettings> const& font_variation_settings, Optional<Gfx::ShapeFeatures> const& shape_features)
{
    return Gfx::FontDatabase::the().get(m_default_font_name, point_size, 400, Gfx::FontWidth::Normal, 0, font_variation_settings, shape_features);
}

Gfx::Font& FontPlugin::default_fixed_width_font()
{
    return *m_default_fixed_width_font;
}

Vector<FlyString> FontPlugin::symbol_font_names()
{
    return m_symbol_font_names;
}

void FontPlugin::update_generic_fonts()
{
    // How we choose which system font to use for each CSS font:
    // 1. Try a list of known-suitable fonts with their names hard-coded below.

    // This is rather weird, but it's how things work right now.
    // We should eventually have a way to query the system for the default font.
    // Furthermore, we should allow overriding via some kind of configuration mechanism.

    m_generic_font_names.resize(static_cast<size_t>(GenericFont::__Count));

    auto update_mapping = [&](GenericFont generic_font, ReadonlySpan<FlyString> fallbacks) {
        if (m_is_layout_test_mode) {
            m_generic_font_names[static_cast<size_t>(generic_font)] = "SerenitySans"_fly_string;
            return;
        }

        RefPtr<Gfx::Font const> gfx_font;

        StringView generic_family_name;
        switch (generic_font) {
        case GenericFont::Cursive:
            generic_family_name = "cursive"sv;
            break;
        case GenericFont::Fantasy:
            generic_family_name = "fantasy"sv;
            break;
        case GenericFont::Monospace:
        case GenericFont::UiMonospace:
            generic_family_name = "monospace"sv;
            break;
        case GenericFont::SansSerif:
        case GenericFont::UiRounded:
        case GenericFont::UiSansSerif:
            generic_family_name = "sans-serif"sv;
            break;
        case GenericFont::Serif:
        case GenericFont::UiSerif:
            generic_family_name = "serif"sv;
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        auto name = Gfx::TypefaceSkia::resolve_generic_family(generic_family_name);
        if (name.has_value()) {
            gfx_font = Gfx::FontDatabase::the().get(name.value(), 16, 400, Gfx::FontWidth::Normal, 0);
        }

        if (!gfx_font) {
            for (auto const& fallback : fallbacks) {
                gfx_font = Gfx::FontDatabase::the().get(fallback, 16, 400, Gfx::FontWidth::Normal, 0);
                if (gfx_font)
                    break;
            }
        }

        m_generic_font_names[static_cast<size_t>(generic_font)] = gfx_font ? gfx_font->family() : String {};
    };

    // Fallback fonts to look for if Gfx::Font can't load expected font
    // The lists are basically arbitrary, taken from https://www.w3.org/Style/Examples/007/fonts.en.html
    // (We also add Android-specific font names to the list from W3 where required.)
    Vector<FlyString> cursive_fallbacks { "Comic Sans MS"_fly_string, "Comic Sans"_fly_string, "Apple Chancery"_fly_string, "Bradley Hand"_fly_string, "Brush Script MT"_fly_string, "Snell Roundhand"_fly_string, "URW Chancery L"_fly_string, "Dancing Script"_fly_string };
    Vector<FlyString> fantasy_fallbacks { "Impact"_fly_string, "Luminari"_fly_string, "Chalkduster"_fly_string, "Jazz LET"_fly_string, "Blippo"_fly_string, "Stencil Std"_fly_string, "Marker Felt"_fly_string, "Trattatello"_fly_string, "Coming Soon"_fly_string };
    Vector<FlyString> monospace_fallbacks { "Andale Mono"_fly_string, "Courier New"_fly_string, "Courier"_fly_string, "FreeMono"_fly_string, "OCR A Std"_fly_string, "Noto Sans Mono"_fly_string, "DejaVu Sans Mono"_fly_string, "Droid Sans Mono"_fly_string, "Liberation Mono"_fly_string };
    Vector<FlyString> sans_serif_fallbacks { "Arial"_fly_string, "Helvetica"_fly_string, "Verdana"_fly_string, "Trebuchet MS"_fly_string, "Gill Sans"_fly_string, "Noto Sans"_fly_string, "Avantgarde"_fly_string, "Optima"_fly_string, "Arial Narrow"_fly_string, "Liberation Sans"_fly_string, "Roboto"_fly_string };
    Vector<FlyString> serif_fallbacks { "Times"_fly_string, "Times New Roman"_fly_string, "Didot"_fly_string, "Georgia"_fly_string, "Palatino"_fly_string, "Bookman"_fly_string, "New Century Schoolbook"_fly_string, "American Typewriter"_fly_string, "Liberation Serif"_fly_string, "Roman"_fly_string, "Noto Serif"_fly_string };

    update_mapping(GenericFont::Cursive, cursive_fallbacks);
    update_mapping(GenericFont::Fantasy, fantasy_fallbacks);
    update_mapping(GenericFont::Monospace, monospace_fallbacks);
    update_mapping(GenericFont::SansSerif, sans_serif_fallbacks);
    update_mapping(GenericFont::Serif, serif_fallbacks);
    update_mapping(GenericFont::UiMonospace, monospace_fallbacks);
    update_mapping(GenericFont::UiRounded, sans_serif_fallbacks);
    update_mapping(GenericFont::UiSansSerif, sans_serif_fallbacks);
    update_mapping(GenericFont::UiSerif, serif_fallbacks);
}

FlyString FontPlugin::generic_font_name(GenericFont generic_font)
{
    return m_generic_font_names[static_cast<size_t>(generic_font)];
}

}
