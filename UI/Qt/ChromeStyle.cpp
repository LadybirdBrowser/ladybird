/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/StringUtils.h>

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

    return is_dark(palette) ? QColor(26, 29, 36) : QColor(244, 246, 248);
}

static QColor chrome_base(QPalette const& palette)
{
    if (palette_matches_current_color_scheme(palette))
        return palette.color(QPalette::Base);

    return is_dark(palette) ? QColor(24, 29, 38) : QColor(255, 255, 255);
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
            .background = QColor(10, 16, 24),
            .surface = QColor(31, 39, 52),
            .recessed = QColor(61, 77, 100),
            .hover = QColor(61, 77, 100),
            .pressed = QColor(78, 95, 120),
            .border = QColor(151, 169, 190),
        };
    }

    return {
        .background = QColor(235, 236, 237),
        .surface = QColor(255, 255, 255),
        .recessed = QColor(154, 158, 164),
        .hover = QColor(228, 229, 231),
        .pressed = QColor(218, 220, 223),
        .border = QColor(94, 98, 103),
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
    return mix(window, material_color_anchors(dark).background, dark ? 0.72 : 0.68);
}

static QColor chrome_tab_strip_background(QPalette const& palette)
{
    auto background = chrome_background(palette);
    auto dark = is_dark(palette);
    return mix(background, dark ? material_color_anchors(dark).background : material_color_anchors(dark).recessed, dark ? 0.075 : 0.30);
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

static QColor chrome_control_border(QPalette const& palette)
{
    auto dark = is_dark(palette);
    if (dark)
        return mix(chrome_control_surface_hover(palette), material_color_anchors(dark).border, 0.42);

    return mix(chrome_border(palette), material_color_anchors(dark).border, 0.18);
}

QColor chrome_active_tab_surface_top(QPalette const& palette)
{
    auto dark = is_dark(palette);
    auto surface = chrome_surface(palette);
    auto active_surface = chrome_surface_hover(palette);
    return dark ? active_surface.lighter(112) : mix(surface, active_surface, 0.22);
}

QColor chrome_active_tab_surface_bottom(QPalette const& palette)
{
    auto dark = is_dark(palette);
    auto surface = chrome_surface(palette);
    auto active_surface = chrome_surface_hover(palette);
    return dark ? mix(surface, active_surface, 0.72) : mix(chrome_background(palette), active_surface, 0.70);
}

QColor chrome_border(QPalette const& palette)
{
    auto dark = is_dark(palette);
    return mix(dark ? chrome_surface(palette) : chrome_background(palette), material_color_anchors(dark).border, 0.22);
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
    auto background = style_sheet_color(chrome_active_tab_surface_top(palette));
    auto background_bottom = style_sheet_color(chrome_active_tab_surface_bottom(palette));
    auto surface_hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto surface_pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto separator = style_sheet_color(mix(chrome_background(palette), chrome_border(palette), is_dark(palette) ? 0.28 : 0.56));
    auto text = style_sheet_color(chrome_button_text(palette));
    auto disabled_text = style_sheet_color(chrome_muted_text(palette));

    return qformatted(R"(
QWidget#LadybirdToolbarContainer {{
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 {0}, stop:1 {1});
    border: 0;
    border-bottom: 1px solid {5};
}}

QWidget#LadybirdNavigationToolbar QToolButton {{
    color: {6};
    background: transparent;
    border: 1px solid transparent;
    border-radius: 18px;
    min-width: 36px;
    min-height: 36px;
    padding: 0;
}}

QWidget#LadybirdNavigationToolbar QToolButton:hover {{
    background: {2};
    border-color: {4};
}}

QWidget#LadybirdNavigationToolbar QToolButton:pressed,
QWidget#LadybirdNavigationToolbar QToolButton:checked {{
    background: {3};
    border-color: {4};
}}

QWidget#LadybirdNavigationToolbar QToolButton:disabled {{
    color: {7};
}}

QWidget#LadybirdNavigationToolbar QToolButton::menu-indicator {{
    image: none;
}}
)",
        background, background_bottom, surface_hover, surface_pressed, control_border, separator, text, disabled_text);
}

QString menu_bar_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_tab_strip_background(palette));
    auto background_bottom = style_sheet_color(mix(chrome_tab_strip_background(palette), QColor(3, 8, 14), is_dark(palette) ? 0.26 : 0.026));
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto text = style_sheet_color(chrome_button_text(palette));
    auto disabled_text = style_sheet_color(chrome_muted_text(palette));
    auto close_hover = style_sheet_color(QColor(196, 43, 28));
    auto close_text = style_sheet_color(QColor(255, 255, 255));

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
    auto surface_color = chrome_surface(palette);
    if (is_dark(palette))
        surface_color = mix(surface_color, material_color_anchors(true).background, 0.42);
    auto hover_color = chrome_surface_hover(palette);
    if (is_dark(palette))
        hover_color = mix(hover_color, material_color_anchors(true).background, 0.24);

    auto surface = style_sheet_color(surface_color);
    auto hover = style_sheet_color(hover_color);
    auto control_hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto focus_border = style_sheet_color(mix(chrome_border(palette), chrome_accent(palette), is_dark(palette) ? 0.46 : 0.58));
    auto text = style_sheet_color(chrome_text(palette));
    auto placeholder = style_sheet_color(chrome_muted_text(palette));
    auto selection = style_sheet_color(chrome_accent(palette));
    auto selection_text = style_sheet_color(palette.color(QPalette::HighlightedText));

    return qformatted(R"(
QLineEdit#LadybirdLocationEdit {{
    color: {4};
    background: {0};
    border: 1px solid {2};
    border-radius: 19px;
    min-height: 36px;
    padding: 0 16px;
    selection-background-color: {6};
    selection-color: {7};
}}

QLineEdit#LadybirdLocationEdit:hover {{
    background: {1};
}}

QLineEdit#LadybirdLocationEdit:focus {{
    background: {1};
    border-color: {3};
}}

QLineEdit#LadybirdLocationEdit:disabled {{
    color: {5};
}}

QToolButton#LadybirdLocationIcon {{
    background: transparent;
    border: 0;
    padding: 0;
}}

QToolButton#LadybirdLocationAction {{
    background: transparent;
    border: 0;
    border-radius: 12px;
    padding: 0;
}}

QToolButton#LadybirdLocationAction:hover {{
    background: {8};
}}
)",
        surface, hover, border, focus_border, text, placeholder, selection, selection_text, control_hover);
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
    padding: 2px 4px;
    spacing: 3px;
}}

QToolBar#LadybirdBookmarksBar QToolButton {{
    color: {2};
    background: transparent;
    border: 1px solid transparent;
    border-radius: 7px;
    padding: 4px 7px;
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
    auto surface = style_sheet_color(chrome_surface(palette));
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto accent = style_sheet_color(chrome_accent(palette));
    auto text = style_sheet_color(chrome_text(palette));
    auto muted = style_sheet_color(chrome_muted_text(palette));

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
        background, surface, hover, pressed, border, control_border, accent, text, muted);
}

QString status_bar_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto hover = style_sheet_color(chrome_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_surface_pressed(palette));
    auto text = style_sheet_color(chrome_text(palette));

    return qformatted(R"(
QWidget#LadybirdStatusBar {{
    background: {0};
    border-top: 1px solid {1};
}}

QWidget#LadybirdStatusBar QPushButton {{
    background: transparent;
    color: {5};
}}

QWidget#LadybirdStatusBar QPushButton:hover {{
    background: {3};
    border-color: {2};
}}

QWidget#LadybirdStatusBar QPushButton:pressed {{
    background: {4};
    border-color: {2};
}}
)",
        background, border, control_border, hover, pressed, text);
}

QString tab_widget_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_tab_strip_background(palette));
    auto background_bottom = style_sheet_color(mix(chrome_tab_strip_background(palette), QColor(3, 8, 14), is_dark(palette) ? 0.26 : 0.026));
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto text = style_sheet_color(chrome_button_text(palette));
    auto close_hover = style_sheet_color(QColor(196, 43, 28));
    auto close_text = style_sheet_color(QColor(255, 255, 255));

    return qformatted(R"(
QWidget#LadybirdTabStrip {{
    color: {5};
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 {0}, stop:1 {1});
    border: 0;
}}

QToolButton#LadybirdNewTabButton,
QPushButton#LadybirdTabButton {{
    color: {5};
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

QPushButton#LadybirdTabButton {{
    min-width: 22px;
    min-height: 22px;
    max-width: 22px;
    max-height: 22px;
}}

QToolButton#LadybirdNewTabButton:hover,
QPushButton#LadybirdTabButton:hover {{
    background: {2};
    border-color: {4};
}}

QToolButton#LadybirdNewTabButton:pressed,
QPushButton#LadybirdTabButton:pressed,
QPushButton#LadybirdTabButton:checked {{
    background: {3};
    border-color: {4};
}}

QToolButton#LadybirdWindowButton,
QToolButton#LadybirdCloseWindowButton {{
    color: {5};
    background: transparent;
    border: 0;
    border-radius: 0;
    min-width: 40px;
    min-height: 40px;
    padding: 0;
}}

QToolButton#LadybirdWindowButton:hover {{
    background: {2};
}}

QToolButton#LadybirdWindowButton:pressed {{
    background: {3};
}}

QToolButton#LadybirdCloseWindowButton:hover {{
    color: {7};
    background: {6};
}}

QToolButton#LadybirdCloseWindowButton:pressed {{
    color: {7};
    background: {6};
}}

QToolButton#LadybirdWindowButton[pressedOutside="true"],
QToolButton#LadybirdCloseWindowButton[pressedOutside="true"] {{
    color: {5};
    background: transparent;
}}
)",
        background, background_bottom, hover, pressed, control_border, text, close_hover, close_text);
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
