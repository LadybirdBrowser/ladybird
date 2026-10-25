/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <UI/Qt/BrandColors.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/StringUtils.h>

#include <QAbstractButton>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyleHints>

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
    auto color_scheme = QGuiApplication::styleHints()->colorScheme();
    if (color_scheme != Qt::ColorScheme::Unknown)
        return color_scheme == Qt::ColorScheme::Dark;

    return palette_is_dark(palette);
}

static bool palette_roles_match_color_scheme(QPalette const& palette, bool dark)
{
    return color_is_dark(palette.color(QPalette::Window)) == dark
        && color_is_dark(palette.color(QPalette::Base)) == dark
        && color_is_dark(palette.color(QPalette::Text)) != dark
        && color_is_dark(palette.color(QPalette::ButtonText)) != dark;
}

static bool palette_matches_current_color_scheme(QPalette const& palette)
{
    auto color_scheme = QGuiApplication::styleHints()->colorScheme();
    if (color_scheme == Qt::ColorScheme::Dark)
        return palette_roles_match_color_scheme(palette, true);
    if (color_scheme == Qt::ColorScheme::Light)
        return palette_roles_match_color_scheme(palette, false);
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
    QColor tab_strip;
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
            .tab_strip = QColor(33, 36, 42),
            .background = QColor(13, 15, 18),
            .surface = QColor(34, 36, 40),
            .recessed = QColor(55, 58, 63),
            .hover = QColor(57, 61, 66),
            .pressed = QColor(70, 74, 80),
            .border = QColor(150, 155, 162),
        };
    }

    return {
        .tab_strip = QColor(195, 197, 204),
        .background = QColor(242, 243, 246),
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

// The tab strip is darker than the toolbar below it, whose color the selected tab shares, so the selected tab stands out.
QColor chrome_tab_strip_background(QPalette const& palette)
{
    auto dark = is_dark(palette);
    return mix(chrome_window(palette), material_color_anchors(dark).tab_strip, dark ? 0.70 : 0.68);
}

// In dark mode, the chrome is lifted from near black toward a cool slate rather than a plain gray.
static constexpr QColor dark_chrome_base { 17, 20, 25 };
static constexpr QColor dark_chrome_tint { 150, 165, 180 };

QColor chrome_background(QPalette const& palette)
{
    auto dark = is_dark(palette);
    if (dark)
        return mix(mix(chrome_window(palette), dark_chrome_base, 0.70), dark_chrome_tint, 0.27);
    return mix(chrome_window(palette), material_color_anchors(dark).background, 0.68);
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

// The active tab shares the color of the toolbar below it.
QColor chrome_active_tab_surface(QPalette const& palette)
{
    return chrome_background(palette);
}

QColor chrome_border(QPalette const& palette)
{
    auto dark = is_dark(palette);
    return mix(dark ? chrome_surface(palette) : chrome_background(palette), material_color_anchors(dark).border, 0.22);
}

QColor chrome_separator(QPalette const& palette)
{
    return mix(chrome_surface(palette), chrome_border(palette), is_dark(palette) ? 0.42 : 0.54);
}

static QColor chrome_content_separator(QPalette const& palette)
{
    return mix(chrome_background(palette), chrome_border(palette), is_dark(palette) ? 0.44 : 0.58);
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

QColor chrome_accent()
{
    return BrandColors::Violet100;
}

static QColor chrome_text_highlight()
{
    return BrandColors::SlateBlue900;
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

static QString style_sheet_color_with_alpha(QColor const& color)
{
    return qformatted("rgba({}, {}, {}, {})", color.red(), color.green(), color.blue(), color.alpha());
}

QString application_style_sheet(QPalette const& palette)
{
    auto surface_color = chrome_surface(palette);
    auto text_color = chrome_text(palette);
    auto surface = style_sheet_color(surface_color);
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto separator = style_sheet_color(chrome_separator(palette));
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
    padding: 5px 24px 5px 14px;
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

QMenu::icon,
QMenu::indicator {{
    left: 8px;
}}

QMenu::indicator,
QMenu::left-arrow,
QMenu::right-arrow {{
    width: 16px;
    height: 16px;
}}

QMenu::right-arrow {{
    right: 8px;
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
    auto dark = is_dark(palette);

    auto background_color = chrome_background(palette);
    auto background = style_sheet_color(background_color);
    auto surface_hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto surface_pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    // In dark mode, the line under the toolbar is as soft as the one under the tab strip.
    auto separator = style_sheet_color(dark ? mix(background_color, chrome_border(palette), 0.44) : chrome_border(palette));
    auto window_controls_separator = style_sheet_color(mix(chrome_background(palette), chrome_border(palette), dark ? 0.36 : 0.46));
    auto text = style_sheet_color(chrome_button_text(palette));
    auto disabled_text = style_sheet_color(chrome_muted_text(palette));
    auto close_hover = style_sheet_color(chrome_destructive_hover());
    auto close_text = style_sheet_color(chrome_destructive_text());
    auto badge_surface_color = QColor(dark ? BrandColors::Indigo900 : BrandColors::Violet10);
    auto badge_border_color = QColor(dark ? BrandColors::Violet50 : BrandColors::Indigo60);
    auto badge_surface = style_sheet_color(badge_surface_color);
    auto badge_border = style_sheet_color(badge_border_color);
    auto badge_surface_hover = style_sheet_color(mix(badge_surface_color, badge_border_color, dark ? 0.28 : 0.22));
    auto badge_surface_pressed = style_sheet_color(mix(badge_surface_color, badge_border_color, dark ? 0.45 : 0.38));

    return qformatted(R"(
QWidget#LadybirdToolbarContainer {{
    background: {0};
    border: 0;
    border-bottom: 1px solid {3};
}}

QWidget#LadybirdToolbarContainer[fullWidthToolbar="true"] {{
    border-bottom: 0;
}}

QWidget#LadybirdToolbarContainer[hoverBookmarksVisible="true"] {{
    border-bottom-color: transparent;
}}

QWidget#LadybirdNavigationToolbar QToolButton {{
    color: {4};
    background: transparent;
    border: 1px solid transparent;
    border-radius: 17px;
    min-width: 32px;
    min-height: 32px;
    padding: 0;
}}

QWidget#LadybirdNavigationToolbar QToolButton:hover {{
    background: transparent;
    border-color: transparent;
}}

QWidget#LadybirdNavigationToolbar QToolButton:pressed,
QWidget#LadybirdNavigationToolbar QToolButton:checked {{
    background: transparent;
    border-color: transparent;
}}

QWidget#LadybirdNavigationToolbar QToolButton:disabled {{
    color: {5};
    background: transparent;
    border-color: transparent;
}}

QWidget#LadybirdNavigationToolbar QToolButton::menu-indicator {{
    image: none;
}}

QPushButton#LadybirdPrivateBadge {{
    color: {4};
    background: {9};
    border: 1px solid {10};
    border-radius: 10px;
    padding: 0 10px;
    font-weight: 600;
}}

QPushButton#LadybirdPrivateBadge:hover {{
    background: {11};
}}

QPushButton#LadybirdPrivateBadge:pressed {{
    background: {12};
}}

QWidget#LadybirdToolbarWindowControlsSeparator {{
    background: {8};
}}

QWidget#LadybirdNavigationToolbar QToolButton#LadybirdWindowButton,
QWidget#LadybirdNavigationToolbar QToolButton#LadybirdCloseWindowButton {{
    color: {4};
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
    color: {7};
    background: {6};
}}

QWidget#LadybirdNavigationToolbar QToolButton#LadybirdCloseWindowButton:pressed {{
    color: {7};
    background: {6};
}}

QWidget#LadybirdNavigationToolbar QToolButton#LadybirdWindowButton[pressedOutside="true"],
QWidget#LadybirdNavigationToolbar QToolButton#LadybirdCloseWindowButton[pressedOutside="true"] {{
    color: {4};
    background: transparent;
}}
)"
#if defined(AK_OS_MACOS)
                      R"(
QWidget#LadybirdToolbarContainer {{
    border-bottom: 0;
}}
)"
#endif
        ,
        background, surface_hover, surface_pressed, separator, text, disabled_text, close_hover, close_text,
        window_controls_separator, badge_surface, badge_border, badge_surface_hover, badge_surface_pressed);
}

QString menu_bar_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_tab_strip_background(palette));
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

QString hamburger_style_sheet(QPalette const& palette)
{
    auto text = style_sheet_color(chrome_button_text(palette));
    auto background = style_sheet_color(chrome_background(palette));
    auto surface_hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto surface_pressed = style_sheet_color(chrome_control_surface_pressed(palette));

    return qformatted(R"(
QWidget#LadybirdHamburgerZoomActions QPushButton#LadybirdHamburgerZoomInButton,
QWidget#LadybirdHamburgerZoomActions QPushButton#LadybirdHamburgerZoomOutButton {{
    color: {0};
    background: {1};
    border-radius: 12px;
}}

QWidget#LadybirdHamburgerZoomActions QPushButton#LadybirdHamburgerResetZoomButton {{
    color: {0};
    background: transparent;
    border-radius: 7px;
}}

QWidget#LadybirdHamburgerZoomActions QPushButton#LadybirdHamburgerZoomInButton:hover,
QWidget#LadybirdHamburgerZoomActions QPushButton#LadybirdHamburgerZoomOutButton:hover,
QWidget#LadybirdHamburgerZoomActions QPushButton#LadybirdHamburgerResetZoomButton:hover {{
    background: {2};
}}

QWidget#LadybirdHamburgerZoomActions QPushButton#LadybirdHamburgerZoomInButton:pressed,
QWidget#LadybirdHamburgerZoomActions QPushButton#LadybirdHamburgerZoomOutButton:pressed,
QWidget#LadybirdHamburgerZoomActions QPushButton#LadybirdHamburgerResetZoomButton:pressed {{
    background: {3};
}}
)",
        text, background, surface_hover, surface_pressed);
}

static QColor autocomplete_surface(QPalette const& palette)
{
    if (is_dark(palette))
        return mix(chrome_background(palette), material_color_anchors(true).recessed, 0.5);
    return chrome_surface(palette);
}

// Hovering a frameless control only lifts it with this soft fill; its ring appears once it is pressed.
static QColor chrome_control_soft_hover(QPalette const& palette)
{
    return is_dark(palette) ? QColor(255, 255, 255, 24) : QColor(0, 0, 0, 16);
}

QColor location_edit_border(QPalette const& palette)
{
    return is_dark(palette) ? mix(chrome_background(palette), chrome_border(palette), 0.36) : chrome_border(palette);
}

static QColor location_edit_focus_border(QPalette const& palette)
{
    return mix(chrome_border(palette), chrome_accent(), is_dark(palette) ? 0.50 : 0.54);
}

QString location_edit_style_sheet(QPalette const& palette)
{
    auto dark = is_dark(palette);
    auto surface_color = chrome_surface(palette);
    if (dark)
        surface_color = mix(chrome_background(palette), material_color_anchors(true).background, 0.62);
    // Hovering the field moves it a little toward the color of the toolbar around it, to show that it can be clicked,
    // while it stays set apart from the toolbar.
    auto hover_color = mix(surface_color, chrome_background(palette), 0.35);
    auto focus_color = dark ? mix(surface_color, QColor(255, 255, 255), 0.035) : surface_color;

    auto border_color = location_edit_border(palette);
    auto hover_border_color = border_color;
    auto focus_border_color = location_edit_focus_border(palette);

    auto surface = style_sheet_color(surface_color);
    auto hover = style_sheet_color(hover_color);
    auto focus = style_sheet_color(focus_color);
    auto popup_surface = style_sheet_color(autocomplete_surface(palette));
    auto border = style_sheet_color(border_color);
    auto hover_border = style_sheet_color(hover_border_color);
    auto focus_border = style_sheet_color(focus_border_color);
    auto text = style_sheet_color(dark ? mix(chrome_text(palette), QColor(255, 255, 255), 0.08) : chrome_text(palette));
    auto placeholder = style_sheet_color(mix(chrome_muted_text(palette), surface_color, dark ? 0.20 : 0.34));
    auto selection = style_sheet_color(chrome_text_highlight());
    auto selection_text = style_sheet_color(Qt::white);
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
    border-radius: {20}px;
    min-height: 32px;
    padding: 0 16px;
    selection-background-color: {6};
    selection-color: {7};
    placeholder-text-color: {5};
}}

QLineEdit#LadybirdLocationEdit[fieldHovered="true"] {{
    background: {1};
    border-color: {8};
}}

QLineEdit#LadybirdLocationEdit:focus {{
    background: {19};
    border-color: {3};
}}

QLineEdit#LadybirdLocationEdit[autocompleteOpen="true"] {{
    background: {21};
    border-bottom-left-radius: 0;
    border-bottom-right-radius: 0;
    border-bottom: 0;
    padding-bottom: 1px;
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
        not_secure_text, not_secure_background, not_secure_hover, not_secure_pressed, not_secure_border, zoom_text, zoom_background, zoom_hover, zoom_pressed, zoom_border, focus, LOCATION_EDIT_CORNER_RADIUS, popup_surface);
}

QString bookmarks_bar_style_sheet(QPalette const& palette, int corner_radius)
{
    auto hover = style_sheet_color_with_alpha(chrome_control_soft_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto text = style_sheet_color(chrome_button_text(palette));
    auto background = style_sheet_color(chrome_background(palette));
    auto border = style_sheet_color(chrome_content_separator(palette));

    return qformatted(R"(
QToolBar#LadybirdBookmarksBar {{
    color: {2};
    background: {4};
    border: 0;
    border-bottom: 1px solid {5};
    padding: 1px 4px;
    spacing: 3px;
}}

QWidget#LadybirdToolbarContainer QToolBar#LadybirdBookmarksBar {{
    border-bottom: 0;
}}

QToolBar#LadybirdBookmarksBar QToolButton {{
    color: {2};
    background: transparent;
    border: 1px solid transparent;
    border-radius: {6}px;
}}

QToolBar#LadybirdBookmarksBar QToolButton:hover {{
    background: {0};
    border-color: transparent;
}}

QToolBar#LadybirdBookmarksBar QToolButton:pressed,
QToolBar#LadybirdBookmarksBar QToolButton:checked {{
    background: {1};
    border-color: {3};
}}
)",
        hover, pressed, text, control_border, background, border, corner_radius);
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
    auto accent = style_sheet_color(chrome_accent());
    auto selection = style_sheet_color(chrome_text_highlight());
    auto selection_text = style_sheet_color(Qt::white);
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
    selection-background-color: {11};
    selection-color: {12};
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
        background, surface, hover, pressed, border, control_border, accent, text, muted, no_results_background, no_results_border, selection, selection_text);
}

QString javascript_dialog_style_sheet(QPalette const& palette)
{
    auto surface = style_sheet_color(chrome_surface(palette));
    auto recessed = style_sheet_color(chrome_surface_recessed(palette));
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto accent = style_sheet_color(chrome_accent());
    auto selection = style_sheet_color(chrome_text_highlight());
    auto selection_text = style_sheet_color(Qt::white);
    auto text = style_sheet_color(chrome_text(palette));
    auto muted = style_sheet_color(chrome_muted_text(palette));
    auto scrim = is_dark(palette) ? QColor(0, 0, 0, 150) : QColor(20, 22, 25, 120);
    auto scrim_color = style_sheet_color_with_alpha(scrim);

    return qformatted(R"(
QWidget#LadybirdJavaScriptDialogOverlay {{
    background: {0};
}}

QFrame#LadybirdJavaScriptDialogPanel {{
    color: {7};
    background: {1};
    border: 1px solid {5};
    border-radius: 12px;
}}

QLabel#LadybirdJavaScriptDialogTitle {{
    color: {7};
    background: transparent;
    border: 0;
    font-size: 15px;
    font-weight: 600;
}}

QScrollArea#LadybirdJavaScriptDialogMessageArea,
QWidget#LadybirdJavaScriptDialogMessageViewport,
QLabel#LadybirdJavaScriptDialogMessage {{
    color: {7};
    background: transparent;
    border: 0;
}}

QFrame#LadybirdJavaScriptDialogPanel QLineEdit {{
    color: {7};
    background: {2};
    border: 1px solid {5};
    border-radius: 8px;
    min-height: 26px;
    padding: 2px 9px;
    selection-background-color: {9};
    selection-color: {10};
}}

QFrame#LadybirdJavaScriptDialogPanel QLineEdit:focus {{
    border-color: {6};
}}

QFrame#LadybirdJavaScriptDialogPanel QPushButton {{
    color: {7};
    background: {2};
    border: 1px solid {5};
    border-radius: 7px;
    min-height: 26px;
    min-width: 72px;
    padding: 2px 12px;
}}

QFrame#LadybirdJavaScriptDialogPanel QPushButton:hover {{
    background: {3};
    border-color: {5};
}}

QFrame#LadybirdJavaScriptDialogPanel QPushButton:pressed {{
    background: {4};
    border-color: {5};
}}

QFrame#LadybirdJavaScriptDialogPanel QPushButton:default {{
    border-color: {6};
}}

QFrame#LadybirdJavaScriptDialogPanel QPushButton:focus {{
    border-color: {6};
}}

QFrame#LadybirdJavaScriptDialogPanel QScrollBar:vertical {{
    background: transparent;
    width: 10px;
    margin: 0;
}}

QFrame#LadybirdJavaScriptDialogPanel QScrollBar::groove:vertical {{
    background: transparent;
}}

QFrame#LadybirdJavaScriptDialogPanel QScrollBar::handle:vertical {{
    background: {8};
    border-radius: 4px;
    min-height: 20px;
}}

QFrame#LadybirdJavaScriptDialogPanel QScrollBar::add-line:vertical,
QFrame#LadybirdJavaScriptDialogPanel QScrollBar::sub-line:vertical {{
    height: 0;
}}

QFrame#LadybirdJavaScriptDialogPanel QScrollBar::add-page:vertical,
QFrame#LadybirdJavaScriptDialogPanel QScrollBar::sub-page:vertical {{
    background: transparent;
}}
)",
        scrim_color, surface, recessed, hover, pressed, control_border, accent, text, muted, selection, selection_text);
}

QString bookmark_dialog_style_sheet(QPalette const& palette)
{
    auto surface = style_sheet_color(chrome_surface(palette));
    auto recessed = style_sheet_color(chrome_surface_recessed(palette));
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto accent = style_sheet_color(chrome_accent());
    auto selection = style_sheet_color(chrome_text_highlight());
    auto selection_text = style_sheet_color(Qt::white);
    auto text = style_sheet_color(chrome_text(palette));

    return qformatted(R"(
QDialog#LadybirdBookmarkDialog {{
    color: {6};
    background: {0};
}}

QDialog#LadybirdBookmarkDialog QLabel {{
    color: {6};
    background: transparent;
    border: 0;
}}

QDialog#LadybirdBookmarkDialog QLineEdit,
QDialog#LadybirdBookmarkDialog QComboBox {{
    color: {6};
    background: {1};
    border: 1px solid {4};
    border-radius: 8px;
    min-height: 26px;
    padding: 2px 9px;
    selection-background-color: {7};
    selection-color: {8};
}}

QDialog#LadybirdBookmarkDialog QLineEdit:focus,
QDialog#LadybirdBookmarkDialog QComboBox:focus {{
    border-color: {5};
}}

QDialog#LadybirdBookmarkDialog QComboBox {{
    padding-right: 24px;
    /* Use the list popup so its rows can be styled on macOS. */
    combobox-popup: 0;
}}

QDialog#LadybirdBookmarkDialog QComboBox::drop-down {{
    subcontrol-origin: padding;
    subcontrol-position: top right;
    width: 24px;
    border: 0;
}}

QDialog#LadybirdBookmarkDialog QComboBox::down-arrow {{
    image: none;
}}

QDialog#LadybirdBookmarkDialog QComboBox QAbstractItemView {{
    color: {6};
    background: {0};
    border: 1px solid {4};
    border-radius: 8px;
    padding: 4px;
    outline: 0;
}}

QDialog#LadybirdBookmarkDialog QComboBox QAbstractItemView::item {{
    min-height: 26px;
    padding: 2px 9px;
    border-radius: 6px;
}}

QDialog#LadybirdBookmarkDialog QComboBox QAbstractItemView::item:selected {{
    color: {6};
    background: {2};
}}

QDialog#LadybirdBookmarkDialog QPushButton {{
    color: {6};
    background: {1};
    border: 1px solid {4};
    border-radius: 7px;
    min-height: 26px;
    min-width: 72px;
    padding: 2px 12px;
}}

QDialog#LadybirdBookmarkDialog QPushButton:hover {{
    background: {2};
    border-color: {4};
}}

QDialog#LadybirdBookmarkDialog QPushButton:pressed {{
    background: {3};
    border-color: {4};
}}

QDialog#LadybirdBookmarkDialog QPushButton:default,
QDialog#LadybirdBookmarkDialog QPushButton:focus {{
    border-color: {5};
}}
)",
        surface, recessed, hover, pressed, control_border, accent, text, selection, selection_text);
}

QString report_review_style_sheet(QPalette const& palette)
{
    auto recessed = style_sheet_color(chrome_surface_recessed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto accent = style_sheet_color(chrome_accent());
    auto selection = style_sheet_color(chrome_text_highlight());
    auto selection_text = style_sheet_color(Qt::white);
    auto text = style_sheet_color(chrome_text(palette));
    auto muted = style_sheet_color(chrome_muted_text(palette));

    return qformatted(R"(
QWidget#LadybirdReportReview QLineEdit,
QWidget#LadybirdReportReview QPlainTextEdit {{
    color: {3};
    background: {0};
    border: 1px solid {1};
    border-radius: 8px;
    selection-background-color: {5};
    selection-color: {6};
}}

QWidget#LadybirdReportReview QLineEdit {{
    min-height: 26px;
    padding: 2px 9px;
}}

QWidget#LadybirdReportReview QPlainTextEdit {{
    padding: 4px 5px;
}}

QWidget#LadybirdReportReview QLineEdit:focus,
QWidget#LadybirdReportReview QPlainTextEdit:focus {{
    border-color: {2};
}}

QWidget#LadybirdReportReview QLineEdit:disabled {{
    color: {4};
}}

QWidget#LadybirdReportReview QFrame#ReportDetails {{
    background: {0};
    border: 1px solid {1};
    border-radius: 8px;
}}
)",
        recessed, control_border, accent, text, muted, selection, selection_text);
}

// Placeholders hint at what to write, so they stay readable rather than taking the platform's faintest text color.
QColor report_review_placeholder_text(QPalette const& palette)
{
    return mix(chrome_text(palette), chrome_surface_recessed(palette), 0.45);
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

QWidget#LadybirdDevToolsBanner QPushButton {{
    border: 1px solid transparent;
    border-radius: 7px;
    min-height: 26px;
    padding: 2px 9px;
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
    auto background = style_sheet_color(chrome_tab_strip_background(palette));
    // The sidebar shares the toolbar's color, so the toolbar and the sidebar frame the page as one surface.
    auto sidebar_background = style_sheet_color(chrome_background_color);
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto text = style_sheet_color(chrome_button_text(palette));
    auto close_hover = style_sheet_color(chrome_destructive_hover());
    auto close_text = style_sheet_color(chrome_destructive_text());
    auto sidebar_separator = style_sheet_color(chrome_content_separator(palette));
    // The sidebar's edge shows up clearly while it can be dragged, to show that the sidebar can be resized.
    auto sidebar_separator_hover = style_sheet_color(mix(chrome_background_color, chrome_text(palette), dark ? 0.30 : 0.28));

    return qformatted(R"(
QWidget#LadybirdTabStrip {{
    color: {4};
    background: {0};
    border: 0;
}}

QWidget#LadybirdVerticalTabBar {{
    color: {4};
    background: {9};
    border: 0;
    border-right: 1px solid {7};
}}

QWidget#LadybirdVerticalTabBar[verticalTabsPosition="right"] {{
    border-right: 0;
    border-left: 1px solid {7};
}}

QWidget#LadybirdVerticalTabBar[hovered="true"],
QWidget#LadybirdVerticalTabBar[active="true"] {{
    border-right: 1px solid {8};
}}

QWidget#LadybirdVerticalTabBar[verticalTabsPosition="right"][hovered="true"],
QWidget#LadybirdVerticalTabBar[verticalTabsPosition="right"][active="true"] {{
    border-right: 0;
    border-left: 1px solid {8};
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
    min-width: 20px;
    min-height: 20px;
    max-width: 20px;
    max-height: 20px;
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
    background: transparent;
    border-color: transparent;
    border-radius: 8px;
}}

QToolButton#LadybirdNewTabButton[verticalTabsButton="false"]:hover {{
    color: {4};
    background: {1};
    border-color: {3};
}}

QPushButton#LadybirdAudioState:hover,
QPushButton#LadybirdTabButton:hover {{
    color: {4};
    background: transparent;
    border-color: transparent;
}}

QToolButton#LadybirdNewTabButton[verticalTabsButton="false"]:pressed {{
    color: {4};
    background: {2};
    border-color: {3};
}}

QPushButton#LadybirdAudioState:pressed,
QPushButton#LadybirdAudioState:checked,
QPushButton#LadybirdTabButton:pressed,
QPushButton#LadybirdTabButton:checked {{
    color: {4};
    background: transparent;
    border-color: transparent;
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
)"
#if defined(AK_OS_MACOS)
                      R"(
QWidget#LadybirdVerticalTabBar {{
    border-right-color: transparent;
}}

QWidget#LadybirdVerticalTabBar[verticalTabsPosition="right"] {{
    border-left-color: transparent;
}}

QWidget#LadybirdVerticalTabsContentSeparator {{
    background: transparent;
}}
)"
#endif
        ,
        background, hover, pressed, control_border, text, close_hover, close_text, sidebar_separator, sidebar_separator_hover, sidebar_background);
}

QString autocomplete_popup_style_sheet(QPalette const& palette)
{
    auto surface = style_sheet_color(autocomplete_surface(palette));
    auto border = style_sheet_color(location_edit_focus_border(palette));
    auto text = style_sheet_color(chrome_text(palette));

    return qformatted(R"(
QFrame#LadybirdAutocompletePopup {{
    color: {2};
    background: {0};
    border: 1px solid {1};
    border-top: 0;
    border-bottom-left-radius: {3}px;
    border-bottom-right-radius: {3}px;
}}

QListView#LadybirdAutocompleteList {{
    color: {2};
    background: {0};
    border: 0;
    outline: 0;
}}
)",
        surface, border, text, LOCATION_EDIT_CORNER_RADIUS);
}

QString downloads_popover_style_sheet(QPalette const& palette)
{
    auto surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface(palette));
    auto recessed_surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface_recessed(palette));
    auto hover_surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface_hover(palette));
    auto border = ChromeStyle::style_sheet_color(ChromeStyle::chrome_border(palette));
    auto text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_text(palette));
    auto muted_text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_muted_text(palette));
    auto accent = ChromeStyle::style_sheet_color(ChromeStyle::chrome_accent());

    return qformatted(R"(
QFrame#LadybirdDownloadsPopover {{
    color: {4};
    background: {0};
    border: 1px solid {3};
    border-radius: 8px;
}}

QScrollArea#LadybirdDownloadsPopoverScroll,
QWidget#LadybirdDownloadsPopoverRows {{
    background: transparent;
    border: 0;
}}

QLabel#LadybirdDownloadsPopoverTitle,
QLabel#LadybirdDownloadFileName {{
    color: {4};
    font-weight: 600;
}}

QLabel#LadybirdDownloadStatus,
QLabel#LadybirdDownloadsEmpty {{
    color: {5};
}}

QFrame#LadybirdDownloadRow {{
    background: {0};
    border: 1px solid {3};
    border-radius: 6px;
}}

QFrame#LadybirdDownloadRow:hover {{
    background: {2};
}}

QProgressBar#LadybirdDownloadProgress {{
    background: {1};
    border: 0;
    border-radius: 2px;
    min-height: 4px;
    max-height: 4px;
}}

QProgressBar#LadybirdDownloadProgress::chunk {{
    background: {6};
    border-radius: 2px;
}}
)",
        surface, recessed_surface, hover_surface, border, text, muted_text, accent);
}

QString message_popover_style_sheet(QPalette const& palette)
{
    auto text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_text(palette));
    auto surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface(palette));
    auto border = ChromeStyle::style_sheet_color(ChromeStyle::chrome_border(palette));
    auto muted_text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_muted_text(palette));
    auto control_border = ChromeStyle::style_sheet_color(ChromeStyle::chrome_control_border(palette));
    auto hover_surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface_hover(palette));
    auto pressed_surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface_pressed(palette));

    auto accent = ChromeStyle::chrome_accent();
    auto accent_color = ChromeStyle::style_sheet_color(accent);
    auto accent_hover = style_sheet_color(BrandColors::Violet500);
    auto accent_pressed = style_sheet_color(BrandColors::Violet900);

    return qformatted(R"(
QFrame#LadybirdMessagePopover {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-radius: 8px;
}}

QLabel#LadybirdMessagePopoverTitle {{
    color: {0};
    font-weight: 600;
}}

QLabel#LadybirdMessagePopoverBody {{
    color: {3};
}}

QPushButton#LadybirdMessagePopoverDismissButton {{
    color: {0};
    background: {1};
    border: 1px solid {4};
    border-radius: 6px;
    padding: 5px 12px;
}}

QPushButton#LadybirdMessagePopoverDismissButton:hover {{
    background: {5};
}}

QPushButton#LadybirdMessagePopoverDismissButton:pressed {{
    background: {6};
}}

QPushButton#LadybirdMessagePopoverAcceptButton {{
    color: #ffffff;
    background: {7};
    border: 1px solid {7};
    border-radius: 6px;
    padding: 5px 12px;
    font-weight: 600;
}}

QPushButton#LadybirdMessagePopoverAcceptButton:hover {{
    background: {8};
    border-color: {8};
}}

QPushButton#LadybirdMessagePopoverAcceptButton:pressed {{
    background: {9};
    border-color: {9};
}}
)",
        text, surface, border, muted_text, control_border, hover_surface, pressed_surface, accent_color, accent_hover, accent_pressed);
}

// The instructions over a screenshot selection look like a message popover, with every action a secondary one.
QString screenshot_selection_style_sheet(QPalette const& palette)
{
    auto text = style_sheet_color(chrome_text(palette));
    auto surface = style_sheet_color(chrome_surface(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto hover_surface = style_sheet_color(chrome_surface_hover(palette));
    auto pressed_surface = style_sheet_color(chrome_surface_pressed(palette));
    auto accent = style_sheet_color(chrome_accent());

    return qformatted(R"(
QFrame#ScreenshotSelectionInstructions {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-radius: 8px;
}}

QFrame#ScreenshotSelectionInstructions QLabel {{
    color: {0};
    background: transparent;
    border: none;
}}

QFrame#ScreenshotSelectionInstructions QPushButton {{
    color: {0};
    background: {1};
    border: 1px solid {3};
    border-radius: 6px;
    padding: 5px 12px;
}}

QFrame#ScreenshotSelectionInstructions QPushButton:hover {{
    background: {4};
}}

QFrame#ScreenshotSelectionInstructions QPushButton:pressed {{
    background: {5};
}}

QFrame#ScreenshotSelectionInstructions QPushButton:focus {{
    border: 2px solid {6};
    padding: 4px 11px;
}}
)",
        text, surface, border, control_border, hover_surface, pressed_surface, accent);
}

// The viewer darkens the whole tab whatever the theme, so its controls are light on dark like a photo viewer's.
QString screenshot_viewer_style_sheet()
{
    return QStringLiteral(R"(
QLabel#ScreenshotViewerCaption {
    color: #ffffff;
    background: rgba(0, 0, 0, 180);
    border: 1px solid rgba(255, 255, 255, 40);
    border-radius: 12px;
    padding: 4px 12px;
}

QPushButton#ScreenshotViewerCloseButton {
    color: #ffffff;
    background: rgba(0, 0, 0, 180);
    border: 1px solid rgba(255, 255, 255, 64);
    border-radius: 6px;
    padding: 5px 12px;
}

QPushButton#ScreenshotViewerCloseButton:hover {
    background: rgba(48, 48, 48, 200);
}

QPushButton#ScreenshotViewerCloseButton:pressed {
    background: rgba(0, 0, 0, 220);
}
)");
}

QString content_blocking_popover_style_sheet(QPalette const& palette)
{
    auto text = style_sheet_color(chrome_text(palette));
    auto surface = style_sheet_color(chrome_surface(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto footer_surface_color = chrome_background(palette);
    auto muted_text = style_sheet_color(mix(chrome_text(palette), footer_surface_color, 0.4));
    auto recessed_surface = style_sheet_color(chrome_surface_recessed(palette));
    auto footer_surface = style_sheet_color(footer_surface_color);

    return qformatted(R"(
QFrame#LadybirdContentBlockingPopover {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-radius: 8px;
}}

QFrame#LadybirdContentBlockingPopover QLabel,
QFrame#LadybirdContentBlockingPopover QCheckBox {{
    color: {0};
}}

QLabel#LadybirdContentBlockingSite,
QLabel#LadybirdContentBlockingCount {{
    font-weight: 600;
}}

QLabel#LadybirdContentBlockingCount {{
    background: {4};
    border-radius: 12px;
    padding: 12px 16px;
}}

QFrame#LadybirdContentBlockingPopover QLabel#LadybirdContentBlockingNote {{
    color: {3};
    background: {5};
    border-top: 1px solid {2};
    border-bottom-left-radius: 7px;
    border-bottom-right-radius: 7px;
    padding: 12px 16px;
}}
)",
        text, surface, border, muted_text, recessed_surface, footer_surface);
}

static void paint_circular_control_frame(QPainter& painter, QRect const& rect, qreal device_pixel_ratio, QColor const& background, QColor const& border)
{
    auto frame_rect = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);

    if (device_pixel_ratio <= 1.0) {
        // Qt's raster paint engine produces visible, jagged steps for a 1 px circular outline at DPR 1.
        // Paint at a higher resolution and downsample to give the edge more coverage levels.
        static constexpr int SUPERSAMPLING_SCALE = 4;

        QImage frame(rect.size() * SUPERSAMPLING_SCALE, QImage::Format_ARGB32_Premultiplied);
        frame.fill(Qt::transparent);

        QPainter frame_painter(&frame);
        frame_painter.setRenderHint(QPainter::Antialiasing, true);
        frame_painter.scale(SUPERSAMPLING_SCALE, SUPERSAMPLING_SCALE);
        frame_painter.setBrush(background);
        frame_painter.setPen(QPen(border, 1));
        frame_painter.drawEllipse(frame_rect);
        frame_painter.end();

        painter.drawImage(rect, frame.scaled(rect.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    } else {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(background);
        painter.setPen(QPen(border, 1));
        painter.drawEllipse(frame_rect);
        painter.restore();
    }
}

void paint_circular_control_frame(QPainter& painter, QAbstractButton const& button, CircularControlFrameStyle frame_style)
{
    if (!button.isEnabled())
        return;

    auto pressed = button.isDown() || button.isChecked();
    auto hovered = button.underMouse();
    if (!hovered && !pressed && frame_style == CircularControlFrameStyle::InteractionOnly)
        return;

    auto const& palette = button.window()->palette();
    auto background = frame_style == CircularControlFrameStyle::ActiveTabOverlay
        ? chrome_active_tab_surface(palette)
        : chrome_control_surface_hover(palette);
    auto border = frame_style == CircularControlFrameStyle::ActiveTabOverlay
        ? chrome_control_surface_hover(palette)
        : chrome_control_border(palette);

    if (hovered && frame_style == CircularControlFrameStyle::InteractionOnly) {
        background = chrome_control_soft_hover(palette);
        border = Qt::transparent;
    } else if (hovered) {
        background = chrome_control_surface_hover(palette);
        border = chrome_control_border(palette);
    }
    if (pressed) {
        // A frameless button can sit on the chrome or on a lighter tab, so pressing it deepens the hover fill rather
        // than painting an opaque color that could be darker than what is below.
        if (frame_style == CircularControlFrameStyle::InteractionOnly)
            background = is_dark(palette) ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 26);
        else
            background = chrome_control_surface_pressed(palette);
        border = chrome_control_border(palette);
    }

    paint_circular_control_frame(painter, button.rect(), button.devicePixelRatioF(), background, border);
}

// A purple gradient with two soft bright spots that moves steadily along a border, to show that something is loading.
// Every swirl follows the same clock, so the swirls on screen move in step.
void paint_loading_swirl(QPainter& painter, QPainterPath const& border)
{
    static QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();
    auto phase = static_cast<qreal>(clock.elapsed() % LOADING_SWIRL_PERIOD_MS) / LOADING_SWIRL_PERIOD_MS;

    // The border is drawn in short segments, each colored by where it sits along the border.
    static constexpr qreal segment_length = 2.0;
    static constexpr qreal bright_spots = 2.0;
    // NB: Without caching, every pointAtPercent() call below measures the whole path again.
    auto path = border;
    path.setCachingEnabled(true);
    auto segments = max(1, static_cast<int>(ceil(path.length() / segment_length)));
    auto color = chrome_accent();

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);

    // NB: pointAtPercent() measures along the path's length, so the swirl moves just as fast around the curves of a
    //     border as along its straight edges.
    auto from_point = path.pointAtPercent(0);
    for (int segment = 0; segment < segments; ++segment) {
        auto from = static_cast<qreal>(segment) / segments;
        auto to = static_cast<qreal>(segment + 1) / segments;
        auto to_point = path.pointAtPercent(to);
        auto wave = 0.5 + (0.5 * cos(2.0 * M_PI * bright_spots * (((from + to) / 2.0) - phase)));
        color.setAlphaF(0.2 + (0.8 * wave * wave));
        painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::FlatCap));
        painter.drawLine(from_point, to_point);
        from_point = to_point;
    }

    painter.restore();
}

}
