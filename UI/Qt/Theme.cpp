/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
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

ChromeColors const& chrome()
{
    if (has_increased_contrast())
        return is_dark() ? dark_increased_contrast_chrome : light_increased_contrast_chrome;
    return is_dark() ? dark_chrome : light_chrome;
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
    return is_dark() ? dark_panel : light_panel;
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

QColor accent()
{
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

QColor primary()
{
    return rgb(0x0a66d0);
}

// A selected row is filled with the primary blue, a little softer on the dark appearance.
QColor selected_row()
{
    return is_dark() ? rgb(0x2463b8) : primary();
}

QColor brand()
{
    return is_dark() ? rgb(0x7a4ce6) : rgb(0x6a39db);
}

}
