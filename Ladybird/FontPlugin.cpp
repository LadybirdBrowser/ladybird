/*
 * Copyright (c) 2022-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FontPlugin.h"
#include <AK/ByteString.h>
#include <AK/String.h>
#include <LibCore/Resource.h>
#include <LibCore/StandardPaths.h>
#include <LibGfx/Font/Emoji.h>
#include <LibGfx/Font/FontDatabase.h>

#ifdef USE_FONTCONFIG
#    include <fontconfig/fontconfig.h>
#endif

namespace Ladybird {

FontPlugin::FontPlugin(bool is_layout_test_mode)
    : m_is_layout_test_mode(is_layout_test_mode)
{
#ifdef USE_FONTCONFIG
    {
        auto fontconfig_initialized = FcInit();
        VERIFY(fontconfig_initialized);
    }
#endif

    // Load anything we can find in the system's font directories
    for (auto const& path : Core::StandardPaths::font_directories().release_value_but_fixme_should_propagate_errors())
        Gfx::FontDatabase::the().load_all_fonts_from_uri(MUST(String::formatted("file://{}", path)));

    auto emoji_path = MUST(Core::Resource::load_from_uri("resource://emoji"sv));
    VERIFY(emoji_path->is_directory());

    Gfx::Emoji::set_emoji_lookup_path(emoji_path->filesystem_path());

    update_generic_fonts();

    auto default_font_name = generic_font_name(Web::Platform::GenericFont::UiSansSerif);
    m_default_font = Gfx::FontDatabase::the().get(default_font_name, 12.0, 400, Gfx::FontWidth::Normal, 0);
    VERIFY(m_default_font);

    auto default_fixed_width_font_name = generic_font_name(Web::Platform::GenericFont::UiMonospace);
    m_default_fixed_width_font = Gfx::FontDatabase::the().get(default_fixed_width_font_name, 12.0, 400, Gfx::FontWidth::Normal, 0);
    VERIFY(m_default_fixed_width_font);
}

FontPlugin::~FontPlugin() = default;

Gfx::Font& FontPlugin::default_font()
{
    return *m_default_font;
}

Gfx::Font& FontPlugin::default_fixed_width_font()
{
    return *m_default_fixed_width_font;
}

#ifdef USE_FONTCONFIG
static Optional<String> query_fontconfig_for_generic_family(Web::Platform::GenericFont generic_font)
{
    char const* pattern_string = nullptr;
    switch (generic_font) {
    case Web::Platform::GenericFont::Cursive:
        pattern_string = "cursive";
        break;
    case Web::Platform::GenericFont::Fantasy:
        pattern_string = "fantasy";
        break;
    case Web::Platform::GenericFont::Monospace:
        pattern_string = "monospace";
        break;
    case Web::Platform::GenericFont::SansSerif:
        pattern_string = "sans-serif";
        break;
    case Web::Platform::GenericFont::Serif:
        pattern_string = "serif";
        break;
    case Web::Platform::GenericFont::UiMonospace:
        pattern_string = "monospace";
        break;
    case Web::Platform::GenericFont::UiRounded:
        pattern_string = "sans-serif";
        break;
    case Web::Platform::GenericFont::UiSansSerif:
        pattern_string = "sans-serif";
        break;
    case Web::Platform::GenericFont::UiSerif:
        pattern_string = "serif";
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    auto* config = FcConfigGetCurrent();
    VERIFY(config);

    FcPattern* pattern = FcNameParse(reinterpret_cast<FcChar8 const*>(pattern_string));
    VERIFY(pattern);

    auto success = FcConfigSubstitute(config, pattern, FcMatchPattern);
    VERIFY(success);

    FcDefaultSubstitute(pattern);

    // Never select bitmap fonts.
    success = FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    VERIFY(success);

    // FIXME: Enable this once we can handle OpenType variable fonts.
    success = FcPatternAddBool(pattern, FC_VARIABLE, FcFalse);
    VERIFY(success);

    Optional<String> name;
    FcResult result {};

    if (auto* matched = FcFontMatch(config, pattern, &result)) {
        FcChar8* family = nullptr;
        if (FcPatternGetString(matched, FC_FAMILY, 0, &family) == FcResultMatch) {
            auto const* family_cstring = reinterpret_cast<char const*>(family);
            if (auto string = String::from_utf8(StringView { family_cstring, strlen(family_cstring) }); !string.is_error()) {
                name = string.release_value();
            }
        }
        FcPatternDestroy(matched);
    }
    FcPatternDestroy(pattern);
    return name;
}
#endif

void FontPlugin::update_generic_fonts()
{
    // How we choose which system font to use for each CSS font:
    // 1. Try a list of known-suitable fonts with their names hard-coded below.

    // This is rather weird, but it's how things work right now.
    // We should eventually have a way to query the system for the default font.
    // Furthermore, we should allow overriding via some kind of configuration mechanism.

    m_generic_font_names.resize(static_cast<size_t>(Web::Platform::GenericFont::__Count));

    auto update_mapping = [&](Web::Platform::GenericFont generic_font, ReadonlySpan<FlyString> fallbacks) {
        if (m_is_layout_test_mode) {
            m_generic_font_names[static_cast<size_t>(generic_font)] = "SerenitySans"_fly_string;
            return;
        }

        RefPtr<Gfx::Font const> gfx_font;

#ifdef USE_FONTCONFIG
        auto name = query_fontconfig_for_generic_family(generic_font);
        if (name.has_value()) {
            gfx_font = Gfx::FontDatabase::the().get(name.value(), 16, 400, Gfx::FontWidth::Normal, 0);
        }
#endif

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
    Vector<FlyString> monospace_fallbacks { "Andale Mono"_fly_string, "Courier New"_fly_string, "Courier"_fly_string, "FreeMono"_fly_string, "OCR A Std"_fly_string, "DejaVu Sans Mono"_fly_string, "Droid Sans Mono"_fly_string, "Liberation Mono"_fly_string };
    Vector<FlyString> sans_serif_fallbacks { "Arial"_fly_string, "Helvetica"_fly_string, "Verdana"_fly_string, "Trebuchet MS"_fly_string, "Gill Sans"_fly_string, "Noto Sans"_fly_string, "Avantgarde"_fly_string, "Optima"_fly_string, "Arial Narrow"_fly_string, "Liberation Sans"_fly_string, "Roboto"_fly_string };
    Vector<FlyString> serif_fallbacks { "Times"_fly_string, "Times New Roman"_fly_string, "Didot"_fly_string, "Georgia"_fly_string, "Palatino"_fly_string, "Bookman"_fly_string, "New Century Schoolbook"_fly_string, "American Typewriter"_fly_string, "Liberation Serif"_fly_string, "Roman"_fly_string, "Noto Serif"_fly_string };

    update_mapping(Web::Platform::GenericFont::Cursive, cursive_fallbacks);
    update_mapping(Web::Platform::GenericFont::Fantasy, fantasy_fallbacks);
    update_mapping(Web::Platform::GenericFont::Monospace, monospace_fallbacks);
    update_mapping(Web::Platform::GenericFont::SansSerif, sans_serif_fallbacks);
    update_mapping(Web::Platform::GenericFont::Serif, serif_fallbacks);
    update_mapping(Web::Platform::GenericFont::UiMonospace, monospace_fallbacks);
    update_mapping(Web::Platform::GenericFont::UiRounded, sans_serif_fallbacks);
    update_mapping(Web::Platform::GenericFont::UiSansSerif, sans_serif_fallbacks);
    update_mapping(Web::Platform::GenericFont::UiSerif, serif_fallbacks);
}

FlyString FontPlugin::generic_font_name(Web::Platform::GenericFont generic_font)
{
    return m_generic_font_names[static_cast<size_t>(generic_font)];
}

#ifdef USE_FONTCONFIG
Vector<String> query_fontconfig_for_fallback_fonts(String const& font_family)
{
    auto* config = FcConfigGetCurrent();
    VERIFY(config);

    FcPattern* pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<FcChar8 const*>(font_family.to_byte_string().characters()));
    VERIFY(pattern);

    auto success = FcConfigSubstitute(config, pattern, FcMatchPattern);
    VERIFY(success);

    FcDefaultSubstitute(pattern);

    // Never select bitmap fonts.
    success = FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    VERIFY(success);

    // FIXME: Enable this once we can handle OpenType variable fonts.
    success = FcPatternAddBool(pattern, FC_VARIABLE, FcFalse);
    VERIFY(success);

    Vector<String> names;
    FcResult result {};

    FcFontSet* font_set = FcFontSort(config, pattern, FcTrue, nullptr, &result);
    if (font_set) {
        for (auto i = 0; i < font_set->nfont; i++) {
            FcChar8* family;
            if (FcPatternGetString(font_set->fonts[i], FC_FAMILY, 0, &family) == FcResultMatch) {
                auto const* family_cstring = reinterpret_cast<char const*>(family);
                if (auto string = String::from_utf8(StringView { family_cstring, strlen(family_cstring) }); !string.is_error()) {
                    names.append(string.release_value());
                }
            }
        }
        FcFontSetDestroy(font_set);
    }
    FcPatternDestroy(pattern);

    return names;
}
#endif

Optional<Vector<String>> FontPlugin::fallback_font_names(String const& font_family)
{
    if (!m_fallback_font_names.contains(font_family)) {
#ifdef USE_FONTCONFIG
        m_fallback_font_names.set(font_family, query_fontconfig_for_fallback_fonts(font_family));
#endif
    }
    return m_fallback_font_names.get(font_family);
}

}
