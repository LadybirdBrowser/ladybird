/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/StringUtils.h>

#include <AK/Platform.h>
#include <QGuiApplication>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#    include <QStyleHints>
#endif

namespace Ladybird::ChromeStyle {

static bool color_is_dark(QColor const& color)
{
    return color.lightness() < 128;
}

static bool palette_is_dark(QPalette const& palette)
{
    return color_is_dark(palette.color(QPalette::Window));
}

bool is_dark(QPalette const& palette)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    auto color_scheme = QGuiApplication::styleHints()->colorScheme();
    if (color_scheme != Qt::ColorScheme::Unknown)
        return color_scheme == Qt::ColorScheme::Dark;
#endif

    return palette_is_dark(palette);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
static bool palette_roles_match_color_scheme(QPalette const& palette, bool dark)
{
    return color_is_dark(palette.color(QPalette::Window)) == dark
        && color_is_dark(palette.color(QPalette::Base)) == dark
        && color_is_dark(palette.color(QPalette::Text)) != dark
        && color_is_dark(palette.color(QPalette::ButtonText)) != dark;
}
#endif

static bool palette_matches_current_color_scheme(QPalette const& palette)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    auto color_scheme = QGuiApplication::styleHints()->colorScheme();
    if (color_scheme == Qt::ColorScheme::Dark)
        return palette_roles_match_color_scheme(palette, true);
    if (color_scheme == Qt::ColorScheme::Light)
        return palette_roles_match_color_scheme(palette, false);
#endif

    Q_UNUSED(palette);
    return true;
}

static QColor chrome_window(QPalette const& palette)
{
    if (palette_matches_current_color_scheme(palette))
        return palette.color(QPalette::Window);

    return is_dark(palette) ? QColor(24, 25, 28) : QColor(245, 245, 246);
}

static QColor chrome_base(QPalette const& palette)
{
    if (palette_matches_current_color_scheme(palette))
        return palette.color(QPalette::Base);

    return is_dark(palette) ? QColor(22, 23, 26) : QColor(255, 255, 255);
}

struct MaterialColorAnchors {
    QColor background;
    QColor surface;
    QColor recessed;
    QColor hover;
    QColor pressed;
    QColor border;
};

static MaterialColorAnchors material_color_anchors(bool dark)
{
    if (dark) {
        return {
            .background = QColor(13, 15, 18),
            .surface = QColor(34, 36, 40),
            .recessed = QColor(55, 58, 63),
            .hover = QColor(57, 61, 66),
            .pressed = QColor(70, 74, 80),
            .border = QColor(150, 155, 162),
        };
    }

    return {
        .background = QColor(236, 236, 237),
        .surface = QColor(255, 255, 255),
        .recessed = QColor(150, 150, 152),
        .hover = QColor(229, 229, 230),
        .pressed = QColor(219, 220, 221),
        .border = QColor(95, 96, 98),
    };
}

QColor mix(QColor const& from, QColor const& to, double amount)
{
    auto channel = [&](int from_channel, int to_channel) {
        return static_cast<int>(from_channel + (to_channel - from_channel) * amount);
    };

    return QColor {
        channel(from.red(), to.red()),
        channel(from.green(), to.green()),
        channel(from.blue(), to.blue()),
    };
}

QColor chrome_background(QPalette const& palette)
{
    auto window = chrome_window(palette);
    auto dark = is_dark(palette);
    return mix(window, material_color_anchors(dark).background, dark ? 0.34 : 0.68);
}

QColor chrome_surface(QPalette const& palette)
{
    auto base = chrome_base(palette);
    auto dark = is_dark(palette);
    return mix(base, material_color_anchors(dark).surface, dark ? 0.64 : 0.72);
}

QColor chrome_surface_recessed(QPalette const& palette)
{
    auto dark = is_dark(palette);
    if (dark)
        return chrome_background(palette).lighter(108);

    return mix(chrome_background(palette), material_color_anchors(dark).recessed, 0.42);
}

QColor chrome_surface_hover(QPalette const& palette)
{
    auto dark = is_dark(palette);
    return mix(chrome_surface(palette), material_color_anchors(dark).hover, dark ? 0.34 : 0.52);
}

QColor chrome_surface_pressed(QPalette const& palette)
{
    auto dark = is_dark(palette);
    return mix(chrome_surface(palette), material_color_anchors(dark).pressed, dark ? 0.48 : 0.56);
}

static QColor chrome_control_surface_hover(QPalette const& palette)
{
    auto dark = is_dark(palette);
    return mix(chrome_surface(palette), material_color_anchors(dark).hover, dark ? 0.82 : 0.62);
}

static QColor chrome_control_surface_pressed(QPalette const& palette)
{
    auto dark = is_dark(palette);
    return mix(chrome_surface(palette), material_color_anchors(dark).pressed, dark ? 0.86 : 0.66);
}

QColor chrome_control_border(QPalette const& palette)
{
    auto dark = is_dark(palette);
    if (dark)
        return mix(chrome_control_surface_hover(palette), material_color_anchors(dark).border, 0.42);

    return mix(chrome_border(palette), material_color_anchors(dark).border, 0.18);
}

QColor chrome_active_tab_surface_top(QPalette const& palette)
{
    auto dark = is_dark(palette);
    if (!dark)
        return QColor(255, 255, 255);

    auto background = chrome_background(palette);
    return mix(background, QColor(255, 255, 255), 0.22);
}

QColor chrome_active_tab_surface_bottom(QPalette const& palette)
{
    auto dark = is_dark(palette);
    if (!dark)
        return QColor(251, 251, 251);

    auto background = chrome_background(palette);
    return mix(background, QColor(255, 255, 255), 0.20);
}

QColor chrome_border(QPalette const& palette)
{
    auto dark = is_dark(palette);
    return mix(dark ? chrome_surface(palette) : chrome_background(palette), material_color_anchors(dark).border, 0.22);
}

QColor chrome_window_outline(QPalette const& palette)
{
    auto dark = is_dark(palette);
    if (dark)
        return chrome_border(palette);

    // The window outline has to hold up against arbitrary backdrops behind the window, not just our own chrome
    // surfaces, so in light mode it is mixed further toward the border anchor than chrome_border().
    return mix(chrome_background(palette), material_color_anchors(false).border, 0.5);
}

QColor chrome_accent(QPalette const& palette)
{
    return palette.color(QPalette::Highlight);
}

QColor chrome_text(QPalette const& palette)
{
    if (palette_matches_current_color_scheme(palette))
        return palette.color(QPalette::Text);

    return is_dark(palette) ? QColor(238, 241, 246) : QColor(24, 29, 36);
}

QColor chrome_button_text(QPalette const& palette)
{
    if (palette_matches_current_color_scheme(palette))
        return palette.color(QPalette::ButtonText);

    return chrome_text(palette);
}

QColor chrome_muted_text(QPalette const& palette)
{
    if (!palette_matches_current_color_scheme(palette))
        return is_dark(palette) ? QColor(154, 163, 176) : QColor(98, 108, 122);

    return palette.color(QPalette::PlaceholderText);
}

static QColor chrome_destructive_hover()
{
    return QColor(196, 43, 28);
}

static QColor chrome_destructive_text()
{
    return QColor(255, 255, 255);
}

QString style_sheet_color(QColor const& color)
{
    return qformatted("rgb({}, {}, {})", color.red(), color.green(), color.blue());
}

QString application_style_sheet(QPalette const& palette)
{
    auto surface_color = chrome_surface(palette);
    auto text_color = chrome_text(palette);
    auto surface = style_sheet_color(surface_color);
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto separator = style_sheet_color(mix(chrome_surface(palette), chrome_border(palette), is_dark(palette) ? 0.42 : 0.54));
    auto text = style_sheet_color(text_color);
    auto disabled_text = style_sheet_color(mix(text_color, surface_color, is_dark(palette) ? 0.58 : 0.48));

    return qformatted(R"(
QMenu {{
    color: {5};
    background: {0};
    border: 1px solid {3};
    border-radius: 7px;
    padding: 5px;
}}

QMenu::item {{
    color: {5};
    background: transparent;
    border-radius: 5px;
    min-height: 20px;
    padding: 5px 14px;
}}

QMenu::item:selected {{
    background: {1};
}}

QMenu::item:pressed {{
    background: {2};
}}

QMenu::item:disabled {{
    color: {6};
}}

QMenu::icon {{
    left: 8px;
}}
)"
#if defined(AK_OS_MACOS)
                      R"(
QMenu::right-arrow,
QMenu::left-arrow {{
    width: 8px;
    height: 8px;
}}
)"
#endif
                      R"(
QMenu::separator {{
    background: {4};
    height: 1px;
    margin: 5px 8px;
}}
)",
        surface, hover, pressed, border, separator, text, disabled_text);
}

QString toolbar_container_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto surface_hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto surface_pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto separator = style_sheet_color(chrome_border(palette));
    auto window_controls_separator = style_sheet_color(mix(chrome_background(palette), chrome_border(palette), is_dark(palette) ? 0.36 : 0.46));
    auto text = style_sheet_color(chrome_button_text(palette));
    auto disabled_text = style_sheet_color(chrome_muted_text(palette));
    auto close_hover = style_sheet_color(chrome_destructive_hover());
    auto close_text = style_sheet_color(chrome_destructive_text());

    return qformatted(R"(
QWidget#LadybirdToolbarContainer {{
    background: {0};
    border: 0;
    border-bottom: 1px solid {4};
}}

QWidget#LadybirdToolbarContainer[fullWidthToolbar="true"] {{
    border-bottom: 0;
}}

QWidget#LadybirdNavigationToolbar QToolButton {{
    color: {5};
    background: transparent;
    border: 1px solid transparent;
    border-radius: 17px;
    min-width: 34px;
    min-height: 34px;
    margin: 1px 0;
    padding: 0;
}}

QWidget#LadybirdNavigationToolbar QToolButton:hover {{
    background: {1};
    border-color: {3};
}}

QWidget#LadybirdNavigationToolbar QToolButton:pressed,
QWidget#LadybirdNavigationToolbar QToolButton:checked {{
    background: {2};
    border-color: {3};
}}

QWidget#LadybirdNavigationToolbar QToolButton:disabled {{
    color: {6};
    background: transparent;
    border-color: transparent;
}}

QWidget#LadybirdNavigationToolbar QToolButton::menu-indicator {{
    image: none;
}}

QWidget#LadybirdToolbarWindowControlsSeparator {{
    background: {9};
}}

QWidget#LadybirdNavigationToolbar QToolButton#LadybirdWindowButton,
QWidget#LadybirdNavigationToolbar QToolButton#LadybirdCloseWindowButton {{
    color: {5};
    background: transparent;
    border: 0;
    border-radius: 0;
    min-width: 38px;
    min-height: 38px;
    max-width: 38px;
    max-height: 38px;
    margin: 0;
    padding: 0;
}}

QWidget#LadybirdNavigationToolbar QToolButton#LadybirdWindowButton:hover {{
    background: {1};
}}

QWidget#LadybirdNavigationToolbar QToolButton#LadybirdWindowButton:pressed {{
    background: {2};
}}

QWidget#LadybirdNavigationToolbar QToolButton#LadybirdCloseWindowButton:hover {{
    color: {8};
    background: {7};
}}

QWidget#LadybirdNavigationToolbar QToolButton#LadybirdCloseWindowButton:pressed {{
    color: {8};
    background: {7};
}}

QWidget#LadybirdNavigationToolbar QToolButton#LadybirdWindowButton[pressedOutside="true"],
QWidget#LadybirdNavigationToolbar QToolButton#LadybirdCloseWindowButton[pressedOutside="true"] {{
    color: {5};
    background: transparent;
}}
)",
        background, surface_hover, surface_pressed, control_border, separator, text, disabled_text, close_hover, close_text, window_controls_separator);
}

QString menu_bar_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto background_bottom = background;
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto text = style_sheet_color(chrome_button_text(palette));
    auto disabled_text = style_sheet_color(chrome_muted_text(palette));
    auto close_hover = style_sheet_color(chrome_destructive_hover());
    auto close_text = style_sheet_color(chrome_destructive_text());

    return qformatted(R"(
QMenuBar#LadybirdMenuBar {{
    color: {5};
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 {0}, stop:1 {1});
    border: 0;
    padding: 0 4px 0 8px;
    spacing: 2px;
}}

QMenuBar#LadybirdMenuBar::item {{
    color: {5};
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 4px 9px;
}}

QMenuBar#LadybirdMenuBar::item:selected {{
    background: {2};
    border-color: {4};
}}

QMenuBar#LadybirdMenuBar::item:pressed {{
    background: {3};
    border-color: {4};
}}

QMenuBar#LadybirdMenuBar::item:disabled {{
    color: {6};
}}

QMenuBar#LadybirdMenuBar QWidget#LadybirdMenuBarWindowControls {{
    background: transparent;
}}

QMenuBar#LadybirdMenuBar QToolButton#LadybirdWindowButton,
QMenuBar#LadybirdMenuBar QToolButton#LadybirdCloseWindowButton {{
    color: {5};
    background: transparent;
    border: 0;
    border-radius: 0;
    min-width: 40px;
    min-height: 30px;
    max-height: 30px;
    padding: 0;
}}

QMenuBar#LadybirdMenuBar QToolButton#LadybirdWindowButton:hover {{
    background: {2};
}}

QMenuBar#LadybirdMenuBar QToolButton#LadybirdWindowButton:pressed {{
    background: {3};
}}

QMenuBar#LadybirdMenuBar QToolButton#LadybirdCloseWindowButton:hover {{
    color: {8};
    background: {7};
}}

QMenuBar#LadybirdMenuBar QToolButton#LadybirdCloseWindowButton:pressed {{
    color: {8};
    background: {7};
}}

QMenuBar#LadybirdMenuBar QToolButton#LadybirdWindowButton[pressedOutside="true"],
QMenuBar#LadybirdMenuBar QToolButton#LadybirdCloseWindowButton[pressedOutside="true"] {{
    color: {5};
    background: transparent;
}}
)",
        background, background_bottom, hover, pressed, control_border, text, disabled_text, close_hover, close_text);
}

QString location_edit_style_sheet(QPalette const& palette)
{
    auto dark = is_dark(palette);
    auto surface_color = chrome_surface(palette);
    if (dark)
        surface_color = mix(chrome_background(palette), material_color_anchors(true).background, 0.34);
    auto hover_color = surface_color;
    auto focus_color = dark ? mix(surface_color, QColor(255, 255, 255), 0.035) : surface_color;

    auto border_color = dark ? mix(chrome_background(palette), chrome_border(palette), 0.36) : chrome_border(palette);
    auto hover_border_color = border_color;
    auto focus_border_color = mix(chrome_border(palette), chrome_accent(palette), dark ? 0.50 : 0.54);

    auto surface = style_sheet_color(surface_color);
    auto hover = style_sheet_color(hover_color);
    auto focus = style_sheet_color(focus_color);
    auto border = style_sheet_color(border_color);
    auto hover_border = style_sheet_color(hover_border_color);
    auto focus_border = style_sheet_color(focus_border_color);
    auto text = style_sheet_color(dark ? mix(chrome_text(palette), QColor(255, 255, 255), 0.08) : chrome_text(palette));
    auto placeholder = style_sheet_color(mix(chrome_muted_text(palette), surface_color, dark ? 0.46 : 0.34));
    auto selection = style_sheet_color(chrome_accent(palette));
    auto selection_text = style_sheet_color(palette.color(QPalette::HighlightedText));
    auto not_secure_text = style_sheet_color(dark ? QColor(224, 142, 136) : QColor(144, 62, 56));
    auto not_secure_background = style_sheet_color(dark ? mix(surface_color, QColor(102, 52, 48), 0.28) : QColor(246, 235, 233));
    auto not_secure_hover = style_sheet_color(dark ? mix(surface_color, QColor(104, 55, 51), 0.34) : QColor(242, 226, 223));
    auto not_secure_pressed = style_sheet_color(dark ? mix(surface_color, QColor(112, 60, 55), 0.40) : QColor(236, 215, 211));
    auto not_secure_border = style_sheet_color(dark ? mix(QColor(92, 48, 45), chrome_border(palette), 0.52) : QColor(224, 203, 199));
    auto zoom_text = style_sheet_color(chrome_muted_text(palette));
    auto zoom_background = style_sheet_color(dark ? mix(surface_color, chrome_surface_recessed(palette), 0.28) : mix(surface_color, chrome_surface_recessed(palette), 0.14));
    auto zoom_hover = style_sheet_color(dark ? mix(surface_color, chrome_surface_recessed(palette), 0.36) : mix(surface_color, chrome_surface_recessed(palette), 0.20));
    auto zoom_pressed = style_sheet_color(dark ? mix(surface_color, chrome_surface_recessed(palette), 0.44) : mix(surface_color, chrome_surface_recessed(palette), 0.28));
    auto zoom_border = style_sheet_color(dark ? mix(chrome_border(palette), surface_color, 0.38) : mix(chrome_border(palette), surface_color, 0.54));

    return qformatted(R"(
QLineEdit#LadybirdLocationEdit {{
    color: {4};
    background: {0};
    border: 1px solid {2};
    border-radius: 16px;
    min-height: 32px;
    padding: 0 16px;
    selection-background-color: {6};
    selection-color: {7};
    placeholder-text-color: {5};
}}

QLineEdit#LadybirdLocationEdit:hover {{
    background: {1};
    border-color: {8};
}}

QLineEdit#LadybirdLocationEdit:focus {{
    background: {19};
    border-color: {3};
}}

QLineEdit#LadybirdLocationEdit:disabled {{
    color: {5};
    border-color: {2};
}}

QToolButton#LadybirdLocationIcon {{
    background: transparent;
    border: 0;
    padding: 0;
}}

QToolButton#LadybirdLocationIcon[notSecure="true"] {{
    color: {9};
    background: {10};
    border: 1px solid {13};
    border-radius: 10px;
    padding: 0 7px;
    font-weight: 500;
}}

QToolButton#LadybirdLocationIcon[notSecure="true"]:hover {{
    background: {11};
}}

QToolButton#LadybirdLocationIcon[notSecure="true"]:pressed {{
    background: {12};
}}

QToolButton#LadybirdLocationZoomIndicator {{
    color: {14};
    background: {15};
    border: 1px solid {18};
    border-radius: 10px;
    padding: 0 7px;
    font-weight: 500;
}}

QToolButton#LadybirdLocationZoomIndicator:hover {{
    background: {16};
}}

QToolButton#LadybirdLocationZoomIndicator:pressed {{
    background: {17};
}}

QToolButton#LadybirdLocationAction {{
    background: transparent;
    border: 0;
    margin: 1px;
    padding: 0;
}}
)",
        surface, hover, border, focus_border, text, placeholder, selection, selection_text, hover_border,
        not_secure_text, not_secure_background, not_secure_hover, not_secure_pressed, not_secure_border, zoom_text, zoom_background, zoom_hover, zoom_pressed, zoom_border, focus);
}

QString bookmarks_bar_style_sheet(QPalette const& palette)
{
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto text = style_sheet_color(chrome_button_text(palette));

    return qformatted(R"(
QToolBar#LadybirdBookmarksBar {{
    color: {2};
    border: 0;
    padding: 1px 4px;
    spacing: 3px;
}}

QToolBar#LadybirdBookmarksBar QToolButton {{
    color: {2};
    background: transparent;
    border: 1px solid transparent;
    border-radius: 7px;
}}

QToolBar#LadybirdBookmarksBar QToolButton:hover {{
    background: {0};
    border-color: {3};
}}

QToolBar#LadybirdBookmarksBar QToolButton:pressed,
QToolBar#LadybirdBookmarksBar QToolButton:checked {{
    background: {1};
    border-color: {3};
}}
)",
        hover, pressed, text, control_border);
}

QString find_in_page_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto surface_color = chrome_surface(palette);
    auto surface = style_sheet_color(surface_color);
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto control_border_color = chrome_control_border(palette);
    auto control_border = style_sheet_color(control_border_color);
    auto accent = style_sheet_color(chrome_accent(palette));
    auto text = style_sheet_color(chrome_text(palette));
    auto muted = style_sheet_color(chrome_muted_text(palette));
    auto no_results_background = style_sheet_color(mix(surface_color, chrome_destructive_hover(), is_dark(palette) ? 0.34 : 0.18));
    auto no_results_border = style_sheet_color(mix(control_border_color, chrome_destructive_hover(), is_dark(palette) ? 0.72 : 0.58));

    return qformatted(R"(
QWidget#LadybirdFindInPageBar {{
    background: {0};
    border-top: 1px solid {4};
}}

QWidget#LadybirdFindInPageBar QLineEdit {{
    color: {7};
    background: {1};
    border: 1px solid {4};
    border-radius: 8px;
    min-height: 26px;
    padding: 2px 9px;
    selection-background-color: {6};
}}

QWidget#LadybirdFindInPageBar QLineEdit:focus {{
    border-color: {6};
}}

QWidget#LadybirdFindInPageBar QLineEdit[noResults="true"] {{
    background: {9};
    border-color: {10};
}}

QWidget#LadybirdFindInPageBar QPushButton {{
    color: {7};
    background: transparent;
    border: 1px solid transparent;
    border-radius: 7px;
    min-height: 26px;
}}

QWidget#LadybirdFindInPageBar QPushButton:hover {{
    background: {2};
    border-color: {5};
}}

QWidget#LadybirdFindInPageBar QPushButton:pressed {{
    background: {3};
    border-color: {5};
}}

QWidget#LadybirdFindInPageBar QCheckBox,
QWidget#LadybirdFindInPageBar QLabel {{
    color: {8};
}}
)",
        background, surface, hover, pressed, border, control_border, accent, text, muted, no_results_background, no_results_border);
}

QString devtools_banner_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto hover = style_sheet_color(chrome_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_surface_pressed(palette));
    auto text = style_sheet_color(chrome_text(palette));

    return qformatted(R"(
QWidget#LadybirdDevToolsBanner {{
    background: {0};
    border-top: 1px solid {1};
}}

QWidget#LadybirdDevToolsBanner QLabel,
QWidget#LadybirdDevToolsBanner QPushButton {{
    background: transparent;
    color: {5};
}}

QWidget#LadybirdDevToolsBanner QPushButton:hover {{
    background: {3};
    border-color: {2};
}}

QWidget#LadybirdDevToolsBanner QPushButton:pressed {{
    background: {4};
    border-color: {2};
}}
)",
        background, border, control_border, hover, pressed, text);
}

QString tab_widget_style_sheet(QPalette const& palette)
{
    auto dark = is_dark(palette);
    auto chrome_background_color = chrome_background(palette);
    auto background = style_sheet_color(chrome_background_color);
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto text = style_sheet_color(chrome_button_text(palette));
    auto close_hover = style_sheet_color(chrome_destructive_hover());
    auto close_text = style_sheet_color(chrome_destructive_text());
    auto strip_separator = style_sheet_color(chrome_border(palette));
    auto sidebar_separator = style_sheet_color(mix(chrome_background_color, chrome_border(palette), dark ? 0.44 : 0.58));
    auto sidebar_separator_hover = style_sheet_color(mix(chrome_background_color, chrome_border(palette), dark ? 0.64 : 0.76));
    auto vertical_tab_button_background_color = style_sheet_color(chrome_active_tab_surface_top(palette));

    return qformatted(R"(
QWidget#LadybirdTabStrip {{
    color: {4};
    background: {0};
    border: 0;
    border-bottom: 1px solid {7};
}}

QWidget#LadybirdVerticalTabBar {{
    color: {4};
    background: {0};
    border-right: 1px solid {8};
}}

QWidget#LadybirdVerticalTabBar[hovered="true"],
QWidget#LadybirdVerticalTabBar[active="true"] {{
    border-right: 1px solid {9};
}}

QWidget#LadybirdVerticalTabsResizeHandle {{
    background: transparent;
    border: 0;
}}

QWidget#LadybirdVerticalTabsContentSeparator {{
    background: {7};
    border: 0;
    min-height: 1px;
    max-height: 1px;
}}

QPushButton#LadybirdAudioState,
QToolButton#LadybirdNewTabButton,
QPushButton#LadybirdTabButton {{
    color: {4};
    background: transparent;
    border: 1px solid transparent;
    border-radius: 11px;
    padding: 0;
}}

QToolButton#LadybirdNewTabButton {{
    min-width: 30px;
    min-height: 30px;
    border-radius: 16px;
}}

QPushButton#LadybirdAudioState,
QPushButton#LadybirdTabButton {{
    min-width: 22px;
    min-height: 22px;
    max-width: 22px;
    max-height: 22px;
}}

QPushButton#LadybirdAudioState[collapsedVerticalTabButton="true"] {{
    min-width: 18px;
    min-height: 18px;
    max-width: 18px;
    max-height: 18px;
    border-radius: 9px;
}}

QPushButton#LadybirdTabButton[collapsedVerticalTabButton="true"] {{
    min-width: 16px;
    min-height: 16px;
    max-width: 16px;
    max-height: 16px;
    background: {10};
    border-color: {1};
    border-radius: 8px;
}}

QPushButton#LadybirdAudioState:hover,
QToolButton#LadybirdNewTabButton[verticalTabsButton="false"]:hover,
QPushButton#LadybirdTabButton:hover {{
    color: {4};
    background: {1};
    border-color: {3};
}}

QPushButton#LadybirdAudioState:pressed,
QPushButton#LadybirdAudioState:checked,
QToolButton#LadybirdNewTabButton[verticalTabsButton="false"]:pressed,
QPushButton#LadybirdTabButton:pressed,
QPushButton#LadybirdTabButton:checked {{
    color: {4};
    background: {2};
    border-color: {3};
}}

QToolButton#LadybirdWindowButton,
QToolButton#LadybirdCloseWindowButton {{
    color: {4};
    background: transparent;
    border: 0;
    border-radius: 0;
    min-width: 40px;
    min-height: 40px;
    padding: 0;
}}

QToolButton#LadybirdWindowButton:hover {{
    background: {1};
}}

QToolButton#LadybirdWindowButton:pressed {{
    background: {2};
}}

QToolButton#LadybirdCloseWindowButton:hover {{
    color: {6};
    background: {5};
}}

QToolButton#LadybirdCloseWindowButton:pressed {{
    color: {6};
    background: {5};
}}

QToolButton#LadybirdWindowButton[pressedOutside="true"],
QToolButton#LadybirdCloseWindowButton[pressedOutside="true"] {{
    color: {4};
    background: transparent;
}}
)",
        background, hover, pressed, control_border, text, close_hover, close_text, strip_separator,
        sidebar_separator, sidebar_separator_hover, vertical_tab_button_background_color);
}

QString autocomplete_popup_style_sheet(QPalette const& palette)
{
    auto surface = style_sheet_color(chrome_surface(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto text = style_sheet_color(chrome_text(palette));

    return qformatted(R"(
QFrame#LadybirdAutocompletePopup {{
    color: {2};
    background: {0};
    border: 1px solid {1};
    border-radius: 8px;
}}

QListView#LadybirdAutocompleteList {{
    color: {2};
    background: transparent;
    border: 0;
    outline: 0;
}}
)",
        surface, border, text);
}

}
