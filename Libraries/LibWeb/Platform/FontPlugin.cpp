/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/HashTable.h>
#include <AK/String.h>
#include <AK/TypeCasts.h>
#include <LibCore/Resource.h>
#include <LibCore/StandardPaths.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/Font/Typeface.h>
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

    auto default_fixed_width_font_name = generic_font_name(GenericFont::UiMonospace, 400, 0);
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
    auto font_name = generic_font_name(GenericFont::UiSansSerif, 400, 0);
    return Gfx::FontDatabase::the().get(font_name, point_size, 400, Gfx::FontWidth::Normal, 0, font_variation_settings, shape_features);
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
    // Store fallback font lists for each generic font category.
    // The actual font selection happens in generic_font_name() based on the requested style.

    m_generic_font_fallbacks.resize(to_underlying(GenericFont::__Count));

    // Fallback fonts to look for if Gfx::Font can't load expected font.
    // The lists are basically arbitrary, taken from https://www.w3.org/Style/Examples/007/fonts.en.html
    // (We also add Android-specific font names to the list from W3 where required.)
    Vector<FlyString> cursive_fallbacks { "Comic Sans MS"_fly_string, "Comic Sans"_fly_string, "Apple Chancery"_fly_string, "Bradley Hand"_fly_string, "Brush Script MT"_fly_string, "Snell Roundhand"_fly_string, "URW Chancery L"_fly_string, "Dancing Script"_fly_string };
    Vector<FlyString> fantasy_fallbacks { "Impact"_fly_string, "Luminari"_fly_string, "Chalkduster"_fly_string, "Jazz LET"_fly_string, "Blippo"_fly_string, "Stencil Std"_fly_string, "Marker Felt"_fly_string, "Trattatello"_fly_string, "Coming Soon"_fly_string };
    Vector<FlyString> monospace_fallbacks { "Andale Mono"_fly_string, "Courier New"_fly_string, "Courier"_fly_string, "FreeMono"_fly_string, "OCR A Std"_fly_string, "Noto Sans Mono"_fly_string, "DejaVu Sans Mono"_fly_string, "Droid Sans Mono"_fly_string, "Liberation Mono"_fly_string };
    Vector<FlyString> sans_serif_fallbacks { "Arial"_fly_string, "Helvetica"_fly_string, "Verdana"_fly_string, "Trebuchet MS"_fly_string, "Gill Sans"_fly_string, "Noto Sans"_fly_string, "Avantgarde"_fly_string, "Optima"_fly_string, "Arial Narrow"_fly_string, "Liberation Sans"_fly_string, "Roboto"_fly_string };
    Vector<FlyString> serif_fallbacks { "Times"_fly_string, "Times New Roman"_fly_string, "Didot"_fly_string, "Georgia"_fly_string, "Palatino"_fly_string, "Bookman"_fly_string, "New Century Schoolbook"_fly_string, "American Typewriter"_fly_string, "Liberation Serif"_fly_string, "Roman"_fly_string, "Noto Serif"_fly_string };

    auto fallback_set = [&](GenericFont font) -> Vector<FlyString>& {
        return m_generic_font_fallbacks[to_underlying(font)];
    };

    fallback_set(GenericFont::Cursive) = move(cursive_fallbacks);
    fallback_set(GenericFont::Fantasy) = move(fantasy_fallbacks);
    fallback_set(GenericFont::Monospace) = move(monospace_fallbacks);
    fallback_set(GenericFont::SansSerif) = move(sans_serif_fallbacks);
    fallback_set(GenericFont::Serif) = move(serif_fallbacks);
    fallback_set(GenericFont::UiMonospace) = fallback_set(GenericFont::Monospace);
    fallback_set(GenericFont::UiRounded) = fallback_set(GenericFont::SansSerif);
    fallback_set(GenericFont::UiSansSerif) = fallback_set(GenericFont::SansSerif);
    fallback_set(GenericFont::UiSerif) = fallback_set(GenericFont::Serif);
}

FlyString FontPlugin::generic_font_name(GenericFont generic_font, int weight, int slope)
{
    if (m_is_layout_test_mode)
        return "SerenitySans"_fly_string;

    GenericFontKey key { generic_font, weight, slope };
    return m_generic_font_cache.ensure(key, [&] {
        return compute_generic_font_name(generic_font, weight, slope);
    });
}

FlyString FontPlugin::compute_generic_font_name(GenericFont generic_font, int weight, int slope)
{
    // https://drafts.csswg.org/css-fonts-4/#generic-font-families
    // User agents should provide reasonable default choices for the generic font families, that express the
    // characteristics of each family as well as possible, within the limits allowed by the underlying technology.
    // NB: We prefer fonts that support the requested weight and slope, falling back to fonts with more style variety.

    auto const& fallbacks = m_generic_font_fallbacks[to_underlying(generic_font)];

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

    auto name = Gfx::TypefaceSkia::resolve_generic_family(generic_family_name, weight, slope);
    if (name.has_value()) {
        if (Gfx::FontDatabase::the().get(name.value(), 16, weight, Gfx::FontWidth::Normal, slope))
            return FlyString { name.release_value() };
    }

    // Score each fallback family based on how well it can satisfy the requested style.
    // Higher score = better match.
    FlyString best_family;
    int best_score = -1;

    for (auto const& family : fallbacks) {
        int score = 0;
        bool has_requested_weight = false;
        bool has_requested_slope = false;
        HashTable<u16> available_weights;

        Gfx::FontDatabase::the().for_each_typeface_with_family_name(family, [&](Gfx::Typeface const& typeface) {
            available_weights.set(typeface.weight());
            if (typeface.weight() == static_cast<u16>(weight))
                has_requested_weight = true;
            if (typeface.slope() == static_cast<u8>(slope))
                has_requested_slope = true;
        });

        // Strongly prefer families that have the exact requested weight.
        if (has_requested_weight)
            score += 1000;

        // Prefer families that have the exact requested slope.
        if (has_requested_slope)
            score += 100;

        // As a tiebreaker, prefer families with more weight variety.
        // This helps select fonts that can handle both regular and bold text.
        score += available_weights.size();

        if (score > best_score) {
            best_score = score;
            best_family = family;
        }
    }

    return best_family;
}

}
