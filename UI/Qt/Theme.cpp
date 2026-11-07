/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Math.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Theme.h>

#include <QAccessibilityHints>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <QWidget>

namespace Ladybird::Theme {

bool is_dark()
{
    auto color_scheme = QGuiApplication::styleHints()->colorScheme();
    if (color_scheme != Qt::ColorScheme::Unknown)
        return color_scheme == Qt::ColorScheme::Dark;
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

bool has_increased_contrast()
{
    return QGuiApplication::styleHints()->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast;
}

static QColor rgb(QRgb value)
{
    return QColor::fromRgb(value);
}

static QColor rgba(int red, int green, int blue, qreal alpha)
{
    return QColor(red, green, blue, qRound(alpha * 255));
}

static ChromeColors const light_chrome {
    .strip = rgb(0xdcdce0),
    .toolbar = rgb(0xffffff),
    .sidebar = rgb(0xdcdce0),
    .field = rgba(0, 0, 0, 0.08),
    .field_focused = rgb(0xffffff),
    .text = rgb(0x1d1d1f),
    .muted = rgb(0x5d5d64),
    .tab_title = rgb(0x3a3a40),
    .icon = rgb(0x3a3a40),
    .disabled = rgb(0xb3b3b9),
    .hover = rgba(0, 0, 0, 0.06),
    .row_title = rgb(0x3f3f45),
    .separator = rgb(0xbcbcc2),
    .divider = rgb(0xd2d2d6),
    .warning = rgb(0xb25d00),
    .warning_fill = rgb(0xfde0b8),
    .warning_ink = rgb(0x6e3300),
    .frame = rgba(0, 0, 0, 0.18),
    .field_edge = Qt::transparent,
    .active_tab_edge = Qt::transparent,
};

static ChromeColors const dark_chrome {
    .strip = rgb(0x141416),
    .toolbar = rgb(0x2e2e31),
    .sidebar = rgb(0x2e2e31),
    .field = rgba(255, 255, 255, 0.08),
    .field_focused = rgb(0x3a3a3e),
    .text = rgb(0xf2f2f5),
    .muted = rgb(0xacacb4),
    .tab_title = rgb(0xacacb4),
    .icon = rgb(0xdcdce1),
    .disabled = rgb(0x5b5b61),
    .hover = rgba(255, 255, 255, 0.06),
    .row_title = rgb(0xcfcfd6),
    .separator = rgb(0x3d3d42),
    .divider = rgb(0x0a0a0c),
    .warning = rgb(0xf2a33a),
    .warning_fill = rgb(0x4f3416),
    .warning_ink = rgb(0xffc46e),
    .frame = rgba(255, 255, 255, 0.10),
    .field_edge = Qt::transparent,
    .active_tab_edge = Qt::transparent,
};

static ChromeColors const private_chrome {
    .strip = rgb(0x1d1430),
    .toolbar = rgb(0x2f2347),
    .sidebar = rgb(0x2f2347),
    .field = rgba(255, 255, 255, 0.09),
    .field_focused = rgb(0x3b2e55),
    .text = rgb(0xf3f0fa),
    .muted = rgb(0xb3aac4),
    .tab_title = rgb(0xb3aac4),
    .icon = rgb(0xe2dcef),
    .disabled = rgb(0x5e566e),
    .hover = rgba(255, 255, 255, 0.07),
    .row_title = rgb(0xd5cee2),
    .separator = rgb(0x4a3d63),
    .divider = rgb(0x0f0a19),
    .warning = rgb(0xf2a33a),
    .warning_fill = rgb(0x4f3416),
    .warning_ink = rgb(0xffc46e),
    .frame = rgba(255, 255, 255, 0.12),
    .field_edge = Qt::transparent,
    .active_tab_edge = Qt::transparent,
};

static ChromeColors const light_increased_contrast_chrome {
    .strip = rgb(0xcfcfd4),
    .toolbar = rgb(0xffffff),
    .sidebar = rgb(0xcfcfd4),
    .field = rgb(0xffffff),
    .field_focused = rgb(0xffffff),
    .text = rgb(0x000000),
    .muted = rgb(0x3a3a40),
    .tab_title = rgb(0x3a3a40),
    .icon = rgb(0x1d1d1f),
    .disabled = rgb(0x8e8e93),
    .hover = rgba(0, 0, 0, 0.10),
    .row_title = rgb(0x1d1d1f),
    .separator = rgb(0x8e8e93),
    .divider = rgb(0x6e6e73),
    .warning = rgb(0x8a4700),
    .warning_fill = rgb(0xfbd29c),
    .warning_ink = rgb(0x5a2900),
    .frame = rgba(0, 0, 0, 0.55),
    .field_edge = rgb(0x6e6e73),
    .active_tab_edge = rgb(0x6e6e73),
};

static ChromeColors const dark_increased_contrast_chrome {
    .strip = rgb(0x000000),
    .toolbar = rgb(0x1c1c1e),
    .sidebar = rgb(0x1c1c1e),
    .field = rgb(0x000000),
    .field_focused = rgb(0x000000),
    .text = rgb(0xffffff),
    .muted = rgb(0xd0d0d6),
    .tab_title = rgb(0xd0d0d6),
    .icon = rgb(0xf2f2f5),
    .disabled = rgb(0x8a8a90),
    .hover = rgba(255, 255, 255, 0.14),
    .row_title = rgb(0xf2f2f5),
    .separator = rgb(0x8a8a90),
    .divider = rgb(0x8a8a90),
    .warning = rgb(0xf2a33a),
    .warning_fill = rgb(0x4f3416),
    .warning_ink = rgb(0xffc46e),
    .frame = rgba(255, 255, 255, 0.55),
    .field_edge = rgb(0x8e8e93),
    .active_tab_edge = rgb(0x8a8a90),
};

static PanelColors const light_panel {
    .surface = rgb(0xfbfbfc),
    .text = rgb(0x1d1d1f),
    .body = rgb(0x3a3a40),
    .muted = rgb(0x5d5d64),
    .shortcut = rgb(0x6e6e75),
    .separator = rgb(0xdcdce0),
    .separator_soft = rgb(0xececef),
    .control = rgba(0, 0, 0, 0.06),
    .pressed = rgba(0, 0, 0, 0.09),
    .page = rgb(0xffffff),
    .edge = rgba(0, 0, 0, 0.10),
    .link = rgb(0x0a66d0),
    .warning = rgb(0x8a4700),
    .badge_background = rgba(106, 57, 219, 0.12),
    .badge_text = rgb(0x4d2bb0),
    .information = rgb(0xdce9fb),
};

static PanelColors const dark_panel {
    .surface = rgb(0x333336),
    .text = rgb(0xf2f2f5),
    .body = rgb(0xdcdce1),
    .muted = rgb(0xacacb4),
    .shortcut = rgb(0x9a9aa2),
    .separator = rgb(0x3d3d42),
    .separator_soft = rgb(0x333337),
    .control = rgba(255, 255, 255, 0.10),
    .pressed = rgba(255, 255, 255, 0.14),
    .page = rgb(0x1c1c1e),
    .edge = rgba(255, 255, 255, 0.12),
    .link = rgb(0x6aa8ff),
    .warning = rgb(0xf2a33a),
    .badge_background = rgba(150, 110, 255, 0.22),
    .badge_text = rgb(0xc9b5ff),
    .information = rgba(10, 122, 255, 0.22),
};

// How saturated the surfaces are in the tint's hue, and the text and icons on them, which take on only a trace of it.
static constexpr float SURFACE_TINT_SATURATION = 0.22f;
static constexpr float INK_TINT_SATURATION = 0.12f;

// A color theme is made from one or two base colors. Its chrome and panel colors are derived from those of the neutral
// theme, by giving each surface the tint's hue while keeping its lightness, so contrast stays as it is.
// NB: Dark violet surfaces mark private windows, so no theme should tint with a deep violet hue. Lavender is bluer and
//     lighter than the private windows' violet, and its dark surfaces are less saturated to stay clear of them.
struct ColorThemeDefinition {
    // The hue the surfaces take on, or none to keep them neutral.
    Optional<QColor> tint;
    // The color of focus rings, selection and selected rows, or none to follow the system's accent color.
    Optional<QColor> accent;
    // How saturated the surfaces of the dark appearance are in the tint's hue.
    float dark_surface_saturation { SURFACE_TINT_SATURATION };
};

// Each tinted theme is made from a single base color, which is both its tint and its accent.
static ColorThemeDefinition color_theme_definition(WebView::ColorTheme theme)
{
    auto from_base_color = [](QRgb base_color) {
        return ColorThemeDefinition { .tint = rgb(base_color), .accent = rgb(base_color) };
    };

    switch (theme) {
    case WebView::ColorTheme::Neutral:
        return {};
    case WebView::ColorTheme::Harbor:
        return from_base_color(0x2f6fd6);
    case WebView::ColorTheme::Glacier:
        return from_base_color(0x1798b8);
    case WebView::ColorTheme::Lagoon:
        return from_base_color(0x12957f);
    case WebView::ColorTheme::Moss:
        return from_base_color(0x3b8f3e);
    case WebView::ColorTheme::Saffron:
        return from_base_color(0xc99500);
    case WebView::ColorTheme::Copper:
        return from_base_color(0xd0631c);
    case WebView::ColorTheme::Cherry:
        return from_base_color(0xd1344b);
    case WebView::ColorTheme::Blossom:
        return from_base_color(0xd2459a);
    case WebView::ColorTheme::Lavender: {
        auto definition = from_base_color(0x7563d4);
        definition.dark_surface_saturation = 0.14f;
        return definition;
    }
    }
    VERIFY_NOT_REACHED();
}

// NB: Translucent fills, such as hovers and the address field, stay neutral, and take on the tint of the surface below.
static void tint(QColor& color, QColor const& tint, float saturation)
{
    if (color.alpha() != 255)
        return;
    color = QColor::fromHslF(tint.hslHueF(), saturation, color.lightnessF());
}

static ChromeColors derive_chrome(ChromeColors const& neutral, Optional<QColor> const& tint_color, float surface_saturation)
{
    auto colors = neutral;
    if (!tint_color.has_value())
        return colors;
    for (auto* surface : { &colors.strip, &colors.toolbar, &colors.sidebar, &colors.field_focused, &colors.separator, &colors.divider })
        tint(*surface, *tint_color, surface_saturation);
    for (auto* ink : { &colors.text, &colors.muted, &colors.tab_title, &colors.icon, &colors.disabled, &colors.row_title })
        tint(*ink, *tint_color, INK_TINT_SATURATION);
    return colors;
}

static PanelColors derive_panel(PanelColors const& neutral, Optional<QColor> const& tint_color, float surface_saturation)
{
    auto colors = neutral;
    if (!tint_color.has_value())
        return colors;
    for (auto* surface : { &colors.surface, &colors.separator, &colors.separator_soft, &colors.page })
        tint(*surface, *tint_color, surface_saturation);
    for (auto* ink : { &colors.text, &colors.body, &colors.muted, &colors.shortcut })
        tint(*ink, *tint_color, INK_TINT_SATURATION);
    return colors;
}

static ColorThemeDefinition s_color_theme;
static ChromeColors s_light_chrome = light_chrome;
static ChromeColors s_dark_chrome = dark_chrome;
static PanelColors s_light_panel = light_panel;
static PanelColors s_dark_panel = dark_panel;

void set_color_theme(WebView::ColorTheme theme)
{
    s_color_theme = color_theme_definition(theme);
    s_light_chrome = derive_chrome(light_chrome, s_color_theme.tint, SURFACE_TINT_SATURATION);
    s_dark_chrome = derive_chrome(dark_chrome, s_color_theme.tint, s_color_theme.dark_surface_saturation);
    s_light_panel = derive_panel(light_panel, s_color_theme.tint, SURFACE_TINT_SATURATION);
    s_dark_panel = derive_panel(dark_panel, s_color_theme.tint, s_color_theme.dark_surface_saturation);
}

// NB: With increased contrast, the chrome stays neutral whatever the color theme.
ChromeColors const& chrome()
{
    if (has_increased_contrast())
        return is_dark() ? dark_increased_contrast_chrome : light_increased_contrast_chrome;
    return is_dark() ? s_dark_chrome : s_light_chrome;
}

static constexpr auto PRIVATE_WINDOW_PROPERTY = "ladybirdPrivateWindow";

// NB: Style sheets give each widget a whole palette of its own, so a window's palette does not reach the widgets in it.
//     The window is marked instead, and a palette marked as private is made for the chrome in it.
void mark_private_window(QWidget& window)
{
    window.setProperty(PRIVATE_WINDOW_PROPERTY, true);
}

// NB: A palette is marked as private by its color for visited links, which nothing in the chrome uses.
QPalette chrome_palette(QWidget const& widget)
{
    auto palette = widget.palette();
    if (auto const* window = widget.window(); window && window->property(PRIVATE_WINDOW_PROPERTY).toBool())
        palette.setColor(QPalette::LinkVisited, private_chrome.strip);
    return palette;
}

bool is_private(QPalette const& palette)
{
    return palette.color(QPalette::LinkVisited) == private_chrome.strip;
}

ChromeColors const& chrome(QPalette const& palette)
{
    return is_private(palette) ? private_chrome : chrome();
}

ChromeColors const& chrome(QWidget const& widget)
{
    return chrome(chrome_palette(widget));
}

bool is_dark(QPalette const& palette)
{
    return is_private(palette) || is_dark();
}

bool is_dark(QWidget const& widget)
{
    return is_dark(chrome_palette(widget));
}

PanelColors const& panel()
{
    return is_dark() ? s_dark_panel : s_light_panel;
}

struct AccentColor {
    int hue;
    QColor light;
    QColor dark;
};

// The accent colors the system offers, each with a variant that clears 3:1 on the light toolbar and one that clears it
// on the dark toolbar. The hues are those of the system's own swatches.
static Array<AccentColor, 7> const accent_colors { {
    { 211, rgb(0x0a7aff), rgb(0x0a7aff) }, // Blue
    { 298, rgb(0xa550a7), rgb(0xc47fc6) }, // Purple
    { 332, rgb(0xe0438f), rgb(0xf2509e) }, // Pink
    { 357, rgb(0xee3b31), rgb(0xee3b31) }, // Red
    { 28, rgb(0xc96400), rgb(0xf7821b) },  // Orange
    { 46, rgb(0x9a7b00), rgb(0xf5c400) },  // Yellow
    { 104, rgb(0x208a3a), rgb(0x30b14f) }, // Green
} };
static AccentColor const graphite_accent { 0, rgb(0x7c7c82), rgb(0x8c8c92) };

static AccentColor const& system_accent_color()
{
    auto system_accent = QGuiApplication::palette().color(QPalette::Accent);
    if (system_accent.hsvSaturation() < 40)
        return graphite_accent;

    auto hue_distance = [&](int hue) {
        auto distance = abs(system_accent.hsvHue() - hue);
        return min(distance, 360 - distance);
    };

    auto const* closest = accent_colors.data();
    for (auto const& accent : accent_colors) {
        if (hue_distance(accent.hue) < hue_distance(closest->hue))
            closest = &accent;
    }
    return *closest;
}

// https://www.w3.org/TR/WCAG21/#dfn-relative-luminance
static float relative_luminance(QColor const& color)
{
    auto linear = [](float channel) { return channel <= 0.04045f ? channel / 12.92f : AK::pow((channel + 0.055f) / 1.055f, 2.4f); };
    return (0.2126f * linear(color.redF())) + (0.7152f * linear(color.greenF())) + (0.0722f * linear(color.blueF()));
}

// https://www.w3.org/TR/WCAG21/#dfn-contrast-ratio
static float contrast_ratio(QColor const& a, QColor const& b)
{
    auto lighter = max(relative_luminance(a), relative_luminance(b));
    auto darker = min(relative_luminance(a), relative_luminance(b));
    return (lighter + 0.05f) / (darker + 0.05f);
}

enum class Adjustment {
    Darken,
    Lighten,
};

// Steps the color's lightness until it has at least the given contrast with the background.
static QColor with_contrast(QColor color, QColor const& background, float minimum_ratio, Adjustment adjustment)
{
    auto step = adjustment == Adjustment::Darken ? -0.02f : 0.02f;
    while (contrast_ratio(color, background) < minimum_ratio) {
        auto lightness = color.lightnessF() + step;
        if (lightness <= 0 || lightness >= 1)
            break;
        color = QColor::fromHslF(color.hslHueF(), color.hslSaturationF(), lightness);
    }
    return color;
}

// The theme's accent clears 3:1 on the toolbar, darker on the light appearance and lighter on the dark one.
QColor accent()
{
    if (s_color_theme.accent.has_value()) {
        auto dark = is_dark();
        return with_contrast(*s_color_theme.accent, chrome().toolbar, 3, dark ? Adjustment::Lighten : Adjustment::Darken);
    }
    auto const& accent = system_accent_color();
    return is_dark() ? accent.dark : accent.light;
}

// On the dark appearance, the focus ring is a lighter tint of the accent, as the accent itself is too deep there.
static QColor focus_color()
{
    auto color = accent();
    if (!is_dark())
        return color;
    auto lighten = [](int channel) { return channel + qRound((255 - channel) * 0.2); };
    return QColor(lighten(color.red()), lighten(color.green()), lighten(color.blue()));
}

QColor focus_edge()
{
    auto color = focus_color();
    if (is_dark())
        color.setAlphaF(0.75f);
    return color;
}

QColor focus_ring()
{
    auto color = focus_color();
    color.setAlphaF(0.16f);
    return color;
}

QColor selection()
{
    auto color = accent();
    color.setAlphaF(is_dark() ? 0.40f : 0.22f);
    return color;
}

// The theme's accent fills primary buttons and selected rows deep enough that white text on them clears 4.5:1.
static QColor accent_fill()
{
    return with_contrast(*s_color_theme.accent, Qt::white, 4.5f, Adjustment::Darken);
}

QColor primary()
{
    if (s_color_theme.accent.has_value())
        return accent_fill();
    return rgb(0x0a66d0);
}

// A selected row is filled with the primary blue, a little softer on the dark appearance.
QColor selected_row()
{
    if (s_color_theme.accent.has_value())
        return accent_fill();
    return is_dark() ? rgb(0x2463b8) : primary();
}

// A tinted theme's logomark clears 4.5:1 on both the strip and the toolbar, as the Ladybird button sits on either,
// depending on where the tabs are.
QColor logomark()
{
    if (!s_color_theme.accent.has_value())
        return is_dark() ? rgb(0xa684ff) : rgb(0x4b22ad);
    auto adjustment = is_dark() ? Adjustment::Lighten : Adjustment::Darken;
    auto color = with_contrast(*s_color_theme.accent, chrome().strip, 4.5f, adjustment);
    return with_contrast(color, chrome().toolbar, 4.5f, adjustment);
}

}
