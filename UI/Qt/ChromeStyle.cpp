/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleHints>
#include <QStyleOption>
#include <QWidget>

namespace Ladybird::ChromeStyle {

bool is_dark(QPalette const& palette)
{
    return Theme::is_dark(palette);
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

// The opaque color of a translucent color painted over an opaque one.
static QColor over(QColor const& color, QColor const& background)
{
    return mix(background, color, static_cast<double>(color.alphaF()));
}

WindowActivity window_activity(QWidget const& widget)
{
    return widget.isActiveWindow() ? WindowActivity::Active : WindowActivity::Inactive;
}

QColor chrome_tab_strip_background(QPalette const& palette)
{
    return Theme::chrome(palette).strip;
}

// NB: The page never sits right against near-black chrome, so the sidebar beside it is the toolbar's color in the dark
//     appearances.
QColor chrome_sidebar_background(QPalette const& palette)
{
    return Theme::chrome(palette).sidebar;
}

QColor chrome_background(QPalette const& palette)
{
    return Theme::chrome(palette).toolbar;
}

QColor chrome_surface(QPalette const&)
{
    return Theme::panel().surface;
}

QColor chrome_surface_recessed(QPalette const&)
{
    auto const& panel = Theme::panel();
    return over(panel.control, panel.surface);
}

QColor chrome_surface_hover(QPalette const&)
{
    auto const& panel = Theme::panel();
    return over(panel.control, panel.surface);
}

QColor chrome_surface_pressed(QPalette const&)
{
    auto const& panel = Theme::panel();
    return over(panel.pressed, panel.surface);
}

static QColor chrome_control_surface_hover(QPalette const& palette)
{
    return chrome_surface_hover(palette);
}

static QColor chrome_control_surface_pressed(QPalette const& palette)
{
    return chrome_surface_pressed(palette);
}

QColor chrome_control_border(QPalette const&)
{
    return Theme::panel().separator;
}

// The active tab shares the color of the toolbar below it.
QColor chrome_active_tab_surface(QPalette const& palette)
{
    return Theme::chrome(palette).toolbar;
}

QColor chrome_border(QPalette const& palette)
{
    return Theme::chrome(palette).divider;
}

QColor chrome_separator(QPalette const& palette)
{
    return Theme::chrome(palette).separator;
}

static QColor chrome_content_separator(QPalette const& palette)
{
    return Theme::chrome(palette).divider;
}

// A window without a frame from the desktop has a hairline of its own. It has to hold up against the chrome inside and
// against whatever is behind the window, so it is firmer than the chrome's other lines.
QColor chrome_window_outline(QPalette const& palette)
{
    auto const& chrome = Theme::chrome(palette);
    return over(is_dark(palette) ? QColor(255, 255, 255, 56) : QColor(0, 0, 0, 56), chrome.strip);
}

QColor chrome_accent()
{
    return Theme::accent();
}

static QColor chrome_text_highlight()
{
    return over(Theme::selection(), Theme::panel().page);
}

QColor chrome_text(QPalette const& palette)
{
    return Theme::chrome(palette).text;
}

QColor chrome_button_text(QPalette const& palette)
{
    return Theme::chrome(palette).icon;
}

QColor chrome_muted_text(QPalette const& palette)
{
    return Theme::chrome(palette).muted;
}

// The titles of tabs and bookmarks share one size, a point smaller than the application's font, which follows the
// system's interface font.
QFont chrome_title_font(QFont font)
{
#if !defined(AK_OS_WINDOWS)
    // NB: The macOS style gives tab bars and tool buttons small fonts of different sizes when it polishes them, so the
    //     titles are sized from the application's font rather than from the font of their widget.
    if (auto application_point_size = QGuiApplication::font().pointSizeF(); application_point_size > 0)
        font.setPointSizeF(application_point_size - 1);
#endif
    return font;
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
    if (color.alpha() < 255)
        return qformatted("rgba({}, {}, {}, {})", color.red(), color.green(), color.blue(), color.alpha());
    return qformatted("rgb({}, {}, {})", color.red(), color.green(), color.blue());
}

QString application_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    auto button_background = over(panel.control, panel.surface);

    // NB: Menus drawn by Qt are drawn by the application style, so that their rows line up on one right edge, which the
    //     style sheet cannot do. Only the widgets put into menus are styled here.
    return qformatted(R"(
QWidget#LadybirdMenuZoomRow QLabel {{
    color: {0};
    background: transparent;
}}

QWidget#LadybirdMenuZoomRow QPushButton {{
    color: {0};
    background: {1};
    border: 0;
    border-radius: 6px;
    min-width: 28px;
    max-width: 28px;
    min-height: 22px;
    max-height: 22px;
}}

QWidget#LadybirdMenuZoomRow QPushButton#LadybirdMenuZoomLevel {{
    background: transparent;
    min-width: 48px;
    max-width: 48px;
}}

QWidget#LadybirdMenuZoomRow QPushButton:hover {{
    background: {2};
}}

QWidget#LadybirdMenuZoomRow QPushButton:pressed {{
    background: {3};
}}
)",
        style_sheet_color(panel.text), style_sheet_color(button_background), style_sheet_color(over(panel.pressed, panel.surface)),
        style_sheet_color(over(panel.pressed, button_background)));
}

QString toolbar_container_style_sheet(QPalette const& palette)
{
    auto dark = is_dark(palette);

    auto background_color = chrome_background(palette);
    auto background = style_sheet_color(background_color);
    auto surface_hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto surface_pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto separator = style_sheet_color(chrome_border(palette));
    auto window_controls_separator = style_sheet_color(mix(chrome_background(palette), chrome_border(palette), dark ? 0.36 : 0.46));
    auto text = style_sheet_color(chrome_button_text(palette));
    auto disabled_text = style_sheet_color(chrome_muted_text(palette));
    auto close_hover = style_sheet_color(chrome_destructive_hover());
    auto close_text = style_sheet_color(chrome_destructive_text());

    return qformatted(R"(
QWidget#LadybirdToolbarContainer {{
    background: {0};
    border: 0;
    border-bottom: 1px solid {3};
}}

QWidget#LadybirdToolbarContainer[bookmarksBarBelow="true"],
QWidget#LadybirdToolbarContainer[verticalTabs="true"] {{
    border-bottom-color: transparent;
}}

QWidget#LadybirdNavigationToolbar QToolButton {{
    color: {4};
    background: transparent;
    border: 0;
    border-radius: 6px;
    min-width: 30px;
    min-height: 28px;
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

QWidget#LadybirdToolbarWindowControlsSeparator {{
    background: {8};
}}

QWidget#LadybirdNavigationToolbar QToolButton#LadybirdWindowButton,
QWidget#LadybirdNavigationToolbar QToolButton#LadybirdCloseWindowButton {{
    color: {4};
    background: transparent;
    border: 0;
    border-radius: 0;
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
)",
        background, surface_hover, surface_pressed, separator, text, disabled_text, close_hover, close_text,
        window_controls_separator);
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

// Hovering a frameless control only lifts it with this soft fill; its ring appears once it is pressed.
QColor location_edit_border(QPalette const& palette)
{
    return Theme::chrome(palette).divider;
}

QString location_edit_style_sheet(QPalette const& palette)
{
    auto const& chrome = Theme::chrome(palette);
    auto const& panel = Theme::panel();

    auto surface_color = over(chrome.field, chrome.toolbar);
    // Hovering the field deepens its fill a little, to show that it can be clicked.
    auto hover_color = over(chrome.hover, surface_color);
    if (chrome.field.alpha() < 255) {
        auto hover_field = chrome.field;
        hover_field.setAlphaF(hover_field.alphaF() + 0.03f);
        hover_color = over(hover_field, chrome.toolbar);
    }
    auto focus_color = chrome.field_focused;
    // The not secure pill deepens a little on hover and press, toward its ink.
    auto warning_hover = mix(chrome.warning_fill, chrome.warning_ink, 0.08);
    auto warning_pressed = mix(chrome.warning_fill, chrome.warning_ink, 0.16);

    auto surface = style_sheet_color(surface_color);
    auto hover = style_sheet_color(hover_color);
    auto focus = style_sheet_color(focus_color);
    auto border = style_sheet_color(chrome.field_edge.alpha() ? chrome.field_edge : surface_color);
    auto hover_border = style_sheet_color(chrome.field_edge.alpha() ? chrome.field_edge : hover_color);
    // NB: The focus ring is drawn right outside the field's edge, so the focused field's own border keeps to its fill.
    auto focus_border = style_sheet_color(chrome.field_edge.alpha() ? chrome.field_edge : focus_color);
    auto text = style_sheet_color(chrome.text);
    auto placeholder = style_sheet_color(chrome.muted);
    auto selection = style_sheet_color(chrome_text_highlight());
    auto selection_text = style_sheet_color(chrome.text);
    auto not_secure_text = style_sheet_color(chrome.warning_ink);
    auto not_secure_background = style_sheet_color(chrome.warning_fill);
    auto not_secure_hover = style_sheet_color(warning_hover);
    auto not_secure_pressed = style_sheet_color(warning_pressed);
    auto zoom_text = style_sheet_color(chrome.muted);
    auto zoom_background = style_sheet_color(over(panel.control, surface_color));
    auto zoom_hover = style_sheet_color(over(panel.pressed, surface_color));
    auto zoom_pressed = style_sheet_color(over(panel.pressed, over(panel.control, surface_color)));

    return qformatted(R"(
QLineEdit#LadybirdLocationEdit {{
    color: {4};
    background: {0};
    border: 1px solid {2};
    border-radius: {18}px;
    min-height: 26px;
    max-height: 26px;
    padding: 0 8px;
    selection-background-color: {6};
    selection-color: {7};
    placeholder-text-color: {5};
}}

QLineEdit#LadybirdLocationEdit[fieldHovered="true"] {{
    background: {1};
    border-color: {8};
}}

QLineEdit#LadybirdLocationEdit:focus {{
    background: {17};
    border-color: {3};
}}

QLineEdit#LadybirdLocationEdit:disabled {{
    color: {5};
    border-color: {2};
}}

QToolButton#LadybirdLocationIcon {{
    background: transparent;
    border: 0;
    border-radius: 5px;
    padding: 0;
}}

QToolButton#LadybirdLocationIcon:hover {{
    background: {19};
}}

QToolButton#LadybirdLocationIcon:pressed {{
    background: {20};
}}

QToolButton#LadybirdLocationIcon[notSecure="true"] {{
    color: {9};
    background: {10};
    border: 0;
    border-radius: 5px;
    padding: 0 8px;
    font-size: 12px;
    font-weight: 500;
}}

QToolButton#LadybirdLocationIcon[notSecure="true"]:hover {{
    background: {11};
}}

QToolButton#LadybirdLocationIcon[notSecure="true"]:pressed {{
    background: {12};
}}

QToolButton#LadybirdLocationZoomIndicator {{
    color: {13};
    background: {14};
    border: 0;
    border-radius: 5px;
    padding: 0 8px;
    font-size: 12px;
}}

QToolButton#LadybirdLocationZoomIndicator:hover {{
    background: {15};
}}

QToolButton#LadybirdLocationZoomIndicator:pressed {{
    background: {16};
}}

QToolButton#LadybirdLocationInUse {{
    color: {13};
    background: {14};
    border: 0;
    border-radius: 5px;
    padding: 0 8px;
    font-size: 12px;
}}

QToolButton#LadybirdLocationInUse:hover {{
    background: {15};
}}

QToolButton#LadybirdLocationInUse:pressed {{
    background: {16};
}}

QToolButton#LadybirdLocationAction {{
    background: transparent;
    border: 0;
    margin: 0;
    padding: 0;
}}

/* NB: The buttons in the field keep their own sizes, rather than the toolbar buttons' minimum size. */
QLineEdit#LadybirdLocationEdit QToolButton {{
    min-width: 0;
    min-height: 0;
}}
)",
        surface, hover, border, focus_border, text, placeholder, selection, selection_text, hover_border,
        not_secure_text, not_secure_background, not_secure_hover, not_secure_pressed, zoom_text, zoom_background, zoom_hover, zoom_pressed, focus, LOCATION_EDIT_CORNER_RADIUS,
        style_sheet_color(chrome.hover), style_sheet_color(panel.pressed));
}

QString bookmarks_bar_style_sheet(QPalette const& palette, int corner_radius)
{
    auto hover = style_sheet_color(Theme::chrome(palette).hover);
    auto pressed = style_sheet_color(Theme::panel().pressed);
    auto control_border = style_sheet_color(Qt::transparent);
    auto text = style_sheet_color(Theme::chrome(palette).text);
    auto background = style_sheet_color(chrome_background(palette));
    auto border = style_sheet_color(chrome_content_separator(palette));

    // NB: The bar is 30 px tall, its 24 px bookmarks two points off its top.
    return qformatted(R"(
QToolBar#LadybirdBookmarksBar {{
    color: {2};
    background: {4};
    border: 0;
    border-bottom: 1px solid {5};
    padding: 2px 8px 4px 8px;
    spacing: 2px;
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

QColor find_in_page_unavailable_control_color()
{
    return Theme::is_dark() ? QColor(0x5b5b61) : QColor(0xb3b3b9);
}

QString find_in_page_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    // Without a match, the field is tinted red, and its count says so in red.
    auto no_match_fill = Theme::is_dark() ? QColor(0x3a2225) : QColor(0xfde8e8);
    auto no_match_text = Theme::is_dark() ? QColor(0xff9a8f) : QColor(0xb3261e);
    return qformatted(R"(
QWidget#LadybirdFindInPageField {{
    background: {0};
    border-radius: 7px;
}}

QWidget#LadybirdFindInPageField QLineEdit {{
    color: {1};
    background: transparent;
    border: 0;
    padding: 0;
    selection-background-color: {6};
    selection-color: {1};
}}

QWidget#LadybirdFindInPageField[noResults="true"] {{
    background: {5};
}}

QLabel#LadybirdFindInPageResult {{
    color: {2};
    font-size: 12px;
}}

QLabel#LadybirdFindInPageResult[noResults="true"] {{
    color: {9};
}}

QPushButton#LadybirdFindInPageButton {{
    color: {3};
    background: transparent;
    border: 0;
    border-radius: 6px;
    font-size: 12px;
    font-weight: 600;
}}

QPushButton#LadybirdFindInPageButton:hover {{
    background: {4};
}}

QPushButton#LadybirdFindInPageButton:checked {{
    color: #ffffff;
    background: {7};
}}

QFrame#LadybirdFindInPageSeparator {{
    background: {8};
    border: 0;
}}
)",
        style_sheet_color(panel.page), style_sheet_color(panel.text), style_sheet_color(panel.muted), style_sheet_color(panel.body),
        style_sheet_color(panel.control), style_sheet_color(no_match_fill), style_sheet_color(Theme::selection()), style_sheet_color(Theme::accent()),
        style_sheet_color(panel.separator), style_sheet_color(no_match_text));
}

QString javascript_dialog_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    auto primary = Theme::primary();
    return qformatted(R"(
QWidget#LadybirdJavaScriptDialogOverlay {{
    background: rgba(0, 0, 0, 56);
}}

QFrame#LadybirdJavaScriptDialogPanel {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-top: 0;
    border-top-left-radius: 0;
    border-top-right-radius: 0;
    border-bottom-left-radius: 12px;
    border-bottom-right-radius: 12px;
}}

QLabel#LadybirdJavaScriptDialogTitle {{
    color: {3};
    background: transparent;
    border: 0;
    font-size: 12px;
}}

QScrollArea#LadybirdJavaScriptDialogMessageArea,
QWidget#LadybirdJavaScriptDialogMessageViewport,
QLabel#LadybirdJavaScriptDialogMessage {{
    color: {0};
    background: transparent;
    border: 0;
    font-size: 13px;
}}

QFrame#LadybirdJavaScriptDialogPanel QLineEdit {{
    color: {0};
    background: {4};
    border: 0;
    border-radius: 7px;
    min-height: 28px;
    padding: 0 9px;
    selection-background-color: {8};
    selection-color: {0};
}}

QCheckBox#LadybirdJavaScriptDialogBlockFurtherDialogs {{
    color: {9};
    background: transparent;
    font-size: 12px;
    spacing: 8px;
}}

QFrame#LadybirdJavaScriptDialogPanel QPushButton {{
    color: {0};
    background: {4};
    border: 0;
    border-radius: 7px;
    min-height: 28px;
    padding: 0 16px;
}}

QFrame#LadybirdJavaScriptDialogPanel QPushButton:pressed {{
    background: {5};
}}

QFrame#LadybirdJavaScriptDialogPanel QPushButton:default {{
    color: #ffffff;
    background: {6};
    font-weight: 500;
}}

QFrame#LadybirdJavaScriptDialogPanel QPushButton:default:pressed {{
    background: {7};
}}

QFrame#LadybirdJavaScriptDialogPanel QScrollBar:vertical {{
    background: transparent;
    width: 10px;
    margin: 0;
}}

QFrame#LadybirdJavaScriptDialogPanel QScrollBar::handle:vertical {{
    background: {3};
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
        style_sheet_color(panel.text), style_sheet_color(panel.surface), style_sheet_color(over(panel.edge, panel.surface)), style_sheet_color(panel.muted),
        style_sheet_color(panel.control), style_sheet_color(panel.pressed), style_sheet_color(primary), style_sheet_color(primary.darker(115)), style_sheet_color(Theme::selection()),
        style_sheet_color(panel.body));
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
    auto selection_text = style_sheet_color(chrome_text(palette));
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
    border: 1px solid transparent;
    border-radius: 7px;
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
    auto selection_text = style_sheet_color(chrome_text(palette));
    auto text = style_sheet_color(chrome_text(palette));
    auto muted = style_sheet_color(chrome_muted_text(palette));

    return qformatted(R"(
QWidget#LadybirdReportReview QLineEdit,
QWidget#LadybirdReportReview QPlainTextEdit {{
    color: {3};
    background: {0};
    border: 1px solid transparent;
    border-radius: 7px;
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

QWidget#LadybirdReportReview QPushButton#WebCompatReportShowReportButton {{
    color: {7};
    background: transparent;
    border: 0;
    padding: 0;
}}

QWidget#LadybirdReportReview QFrame#WebCompatReportFooterLine {{
    background: {8};
    border: 0;
    min-height: 1px;
    max-height: 1px;
}}
)",
        recessed, control_border, accent, text, muted, selection, selection_text, style_sheet_color(Theme::panel().link), style_sheet_color(Theme::panel().separator_soft));
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
    auto tab_strip_background = chrome_tab_strip_background(palette);
    auto background = style_sheet_color(tab_strip_background);
    auto hover = style_sheet_color(chrome_control_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_control_surface_pressed(palette));
    auto control_border = style_sheet_color(chrome_control_border(palette));
    auto text = style_sheet_color(chrome_button_text(palette));
    auto close_hover = style_sheet_color(chrome_destructive_hover());
    auto close_text = style_sheet_color(chrome_destructive_text());
    auto sidebar_separator = style_sheet_color(chrome_content_separator(palette));
    // The sidebar's edge shows up clearly while it can be dragged, to show that the sidebar can be resized.
    auto sidebar_separator_hover = style_sheet_color(mix(tab_strip_background, chrome_text(palette), dark ? 0.30 : 0.28));

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

QToolButton#LadybirdVerticalTabsToggleButton {{
    background: transparent;
    border: 0;
    padding: 0;
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

)"
#endif
        ,
        background, hover, pressed, control_border, text, close_hover, close_text, sidebar_separator, sidebar_separator_hover,
        style_sheet_color(chrome_sidebar_background(palette)));
}

QString autocomplete_popup_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    return qformatted(R"(
QListView#LadybirdAutocompleteList {{
    color: {0};
    background: transparent;
    border: 0;
    outline: 0;
}}
)",
        style_sheet_color(panel.text));
}

QString downloads_popover_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    return qformatted(R"(
QFrame#LadybirdDownloadsPopover {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-radius: 12px;
}}

QScrollArea#LadybirdDownloadsPopoverScroll,
QWidget#LadybirdDownloadsPopoverRows {{
    background: transparent;
    border: 0;
}}

QLabel#LadybirdDownloadsPopoverTitle {{
    color: {0};
    font-weight: 600;
}}

QLabel#LadybirdDownloadFileName {{
    color: {0};
}}

QLabel#LadybirdDownloadStatus,
QLabel#LadybirdDownloadsEmpty {{
    color: {3};
    font-size: 12px;
}}

QLabel#LadybirdDownloadStatus[failed="true"] {{
    color: {6};
}}

QFrame#LadybirdDownloadRow {{
    background: transparent;
    border: 0;
    border-radius: 6px;
}}

QFrame#LadybirdDownloadRow:hover {{
    background: {7};
}}

QProgressBar#LadybirdDownloadProgress {{
    background: {4};
    border: 0;
    border-radius: 2px;
    min-height: 4px;
    max-height: 4px;
}}

QProgressBar#LadybirdDownloadProgress::chunk {{
    background: {8};
    border-radius: 2px;
}}

QFrame#LadybirdPopoverSeparator {{
    background: {4};
    border: 0;
}}

QPushButton#LadybirdPopoverLink {{
    color: {5};
    background: transparent;
    border: 0;
    border-radius: 5px;
    padding: 4px 6px;
    font-size: 12px;
}}

QPushButton#LadybirdPopoverLink:hover,
QPushButton#LadybirdPopoverRow:hover {{
    background: {7};
}}

QPushButton#LadybirdPopoverRow {{
    color: {0};
    background: transparent;
    border: 0;
    border-radius: 6px;
    min-height: 32px;
    padding: 0 10px;
    text-align: left;
}}

QToolButton#LadybirdDownloadAction {{
    background: transparent;
    border: 0;
    border-radius: 7px;
}}

QToolButton#LadybirdDownloadAction:hover {{
    background: {7};
}}

QToolButton#LadybirdDownloadAction:pressed {{
    background: {9};
}}
)",
        style_sheet_color(panel.text), style_sheet_color(panel.surface), style_sheet_color(over(panel.edge, panel.surface)), style_sheet_color(panel.muted),
        style_sheet_color(panel.separator), style_sheet_color(panel.link), style_sheet_color(panel.warning), style_sheet_color(panel.control), style_sheet_color(Theme::accent()),
        style_sheet_color(panel.pressed));
}

QString message_popover_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    auto primary = Theme::primary();
    return qformatted(R"(
QFrame#LadybirdMessagePopover {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-radius: 12px;
}}

QLabel#LadybirdMessagePopoverTitle {{
    color: {0};
    font-weight: 600;
}}

QLabel#LadybirdMessagePopoverIcon {{
    background: {9};
    border-radius: 8px;
}}

QLabel#LadybirdMessagePopoverBody {{
    color: {3};
    font-size: 12px;
}}

QPushButton#LadybirdMessagePopoverDismissButton {{
    color: {0};
    background: {4};
    border: 0;
    border-radius: 7px;
    min-height: 28px;
    padding: 0 14px;
}}

QPushButton#LadybirdMessagePopoverDismissButton:hover,
QPushButton#LadybirdMessagePopoverDismissButton:pressed {{
    background: {5};
}}

QPushButton#LadybirdMessagePopoverAcceptButton {{
    color: #ffffff;
    background: {6};
    border: 0;
    border-radius: 7px;
    min-height: 28px;
    padding: 0 14px;
    font-weight: 500;
}}

QPushButton#LadybirdMessagePopoverAcceptButton:hover {{
    background: {7};
}}

QPushButton#LadybirdMessagePopoverAcceptButton:pressed {{
    background: {8};
}}
)",
        style_sheet_color(panel.text), style_sheet_color(panel.surface), style_sheet_color(over(panel.edge, panel.surface)), style_sheet_color(panel.muted),
        style_sheet_color(panel.control), style_sheet_color(panel.pressed), style_sheet_color(primary), style_sheet_color(primary.darker(110)), style_sheet_color(primary.darker(122)),
        style_sheet_color(panel.information));
}

QString bookmark_popover_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    auto primary = Theme::primary();
    return qformatted(R"(
QFrame#LadybirdBookmarkPopover {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-radius: 12px;
}}

QFrame#LadybirdBookmarkPopover QLabel {{
    background: transparent;
}}

QLabel#LadybirdBookmarkPopoverTitle {{
    color: {0};
    font-size: 13px;
    font-weight: 600;
}}

QLabel#LadybirdBookmarkPopoverLabel {{
    color: {3};
    font-size: 12px;
}}

QLineEdit#LadybirdBookmarkPopoverName {{
    color: {0};
    background: {4};
    border: 1px solid transparent;
    border-radius: 7px;
    min-height: 28px;
    max-height: 28px;
    padding: 0 8px;
    font-size: 13px;
    selection-background-color: {8};
}}

QLineEdit#LadybirdBookmarkPopoverName:focus {{
    border-color: {9};
}}

QComboBox#LadybirdBookmarkPopoverFolder {{
    color: {0};
    background: {5};
    border: 0;
    border-radius: 7px;
    min-height: 28px;
    max-height: 28px;
    padding: 0 9px;
    font-size: 13px;
}}

QComboBox#LadybirdBookmarkPopoverFolder::drop-down {{
    border: 0;
    width: 24px;
}}

QComboBox#LadybirdBookmarkPopoverFolder::down-arrow {{
    image: none;
}}

QPushButton#LadybirdBookmarkPopoverRemove {{
    color: {0};
    background: {5};
    border: 0;
    border-radius: 7px;
    min-height: 28px;
    padding: 0 12px;
    font-size: 13px;
}}

QPushButton#LadybirdBookmarkPopoverRemove:pressed {{
    background: {10};
}}

QPushButton#LadybirdBookmarkPopoverDone {{
    color: #ffffff;
    background: {6};
    border: 0;
    border-radius: 7px;
    min-height: 28px;
    padding: 0 16px;
    font-size: 13px;
    font-weight: 500;
}}

QPushButton#LadybirdBookmarkPopoverDone:pressed {{
    background: {7};
}}
)",
        style_sheet_color(panel.text), style_sheet_color(panel.surface), style_sheet_color(over(panel.edge, panel.surface)), style_sheet_color(panel.muted),
        style_sheet_color(panel.page), style_sheet_color(panel.control), style_sheet_color(primary), style_sheet_color(primary.darker(122)),
        style_sheet_color(Theme::selection()), style_sheet_color(Theme::focus_edge()), style_sheet_color(panel.pressed));
}

// A permission prompt looks like a message popover, its three answers stacked as equal secondary buttons, since the
// browser recommends none of them.
QString permission_prompt_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    return qformatted(R"(
QFrame#LadybirdPermissionPrompt {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-radius: 12px;
}}

QLabel#LadybirdPermissionPromptTitle {{
    color: {0};
    font-weight: 600;
}}

QLabel#LadybirdPermissionPromptIcon {{
    background: {3};
    border-radius: 8px;
}}

QLabel#LadybirdPermissionPromptHint {{
    color: {4};
    font-size: 12px;
}}

QPushButton#LadybirdPermissionPromptAllowAlways,
QPushButton#LadybirdPermissionPromptAllowOnce,
QPushButton#LadybirdPermissionPromptBlock {{
    color: {0};
    background: {5};
    border: 0;
    border-radius: 7px;
    min-height: 28px;
    padding: 0 14px;
    font-size: 13px;
}}

QPushButton#LadybirdPermissionPromptAllowAlways:hover,
QPushButton#LadybirdPermissionPromptAllowOnce:hover,
QPushButton#LadybirdPermissionPromptBlock:hover,
QPushButton#LadybirdPermissionPromptAllowAlways:pressed,
QPushButton#LadybirdPermissionPromptAllowOnce:pressed,
QPushButton#LadybirdPermissionPromptBlock:pressed {{
    background: {6};
}}

QPushButton#LadybirdPermissionPromptAllowAlways:focus,
QPushButton#LadybirdPermissionPromptAllowOnce:focus,
QPushButton#LadybirdPermissionPromptBlock:focus {{
    border: 2px solid {7};
    padding: 0 12px;
}}
)",
        style_sheet_color(panel.text), style_sheet_color(panel.surface), style_sheet_color(over(panel.edge, panel.surface)), style_sheet_color(panel.information),
        style_sheet_color(panel.muted), style_sheet_color(panel.control), style_sheet_color(panel.pressed), style_sheet_color(Theme::focus_ring()));
}

// The instructions over a screenshot selection look like a message popover, with every action a secondary one.
QString screenshot_selection_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    return qformatted(R"(
QFrame#ScreenshotSelectionInstructions {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-radius: 12px;
}}

QFrame#ScreenshotSelectionInstructions QLabel {{
    color: {0};
    background: transparent;
    border: none;
    font-size: 13px;
}}

QFrame#ScreenshotSelectionInstructions QPushButton {{
    color: {0};
    background: {3};
    border: 0;
    border-radius: 7px;
    min-height: 28px;
    max-height: 28px;
    padding: 0 12px;
    font-size: 13px;
}}

QFrame#ScreenshotSelectionInstructions QPushButton:pressed {{
    background: {4};
}}

QFrame#ScreenshotSelectionInstructions QPushButton:focus {{
    border: 2px solid {5};
    padding: 0 10px;
}}

QFrame#ScreenshotSelectionInstructions QPushButton#ScreenshotSelectionCancelButton {{
    background: transparent;
    border: 2px solid transparent;
    padding: 4px;
}}

QFrame#ScreenshotSelectionInstructions QPushButton#ScreenshotSelectionCancelButton:pressed {{
    background: {4};
}}

QFrame#ScreenshotSelectionInstructions QPushButton#ScreenshotSelectionCancelButton:focus {{
    border-color: {5};
}}
)",
        style_sheet_color(panel.text), style_sheet_color(panel.surface), style_sheet_color(over(panel.edge, panel.surface)), style_sheet_color(panel.control),
        style_sheet_color(panel.pressed), style_sheet_color(Theme::focus_ring()));
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

QString site_settings_popover_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    return qformatted(R"(
QFrame#LadybirdSiteSettingsPopover {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-radius: 12px;
}}

QFrame#LadybirdSiteSettingsPopover QLabel {{
    color: {0};
    background: transparent;
}}

QLabel#LadybirdSiteSettingsFavicon {{
    background: #ffffff;
    border: 1px solid #d0d0d6;
    border-radius: 6px;
}}

QLabel#LadybirdSiteSettingsHost {{
    font-weight: 600;
}}

QFrame#LadybirdSiteSettingsPopover QLabel#LadybirdSiteSettingsDetail {{
    color: {3};
    font-size: 12px;
}}

QFrame#LadybirdSiteSettingsPopover QLabel#LadybirdSiteSettingsSecurity {{
    color: {6};
    font-size: 12px;
}}

QFrame#LadybirdPopoverSeparator {{
    background: {4};
    border: 0;
}}

QPushButton#LadybirdPopoverValueButton {{
    color: {0};
    background: {7};
    border: 0;
    border-radius: 6px;
    min-height: 24px;
    max-height: 24px;
    padding: 0 6px 0 9px;
    font-size: 12px;
}}

QPushButton#LadybirdPopoverValueButton:hover,
QPushButton#LadybirdPopoverValueButton:pressed {{
    background: {8};
}}

QPushButton#LadybirdPopoverLink {{
    color: {5};
    background: transparent;
    border: 0;
    border-radius: 5px;
    padding: 6px;
    font-size: 12px;
}}

QPushButton#LadybirdPopoverLink:hover {{
    background: {7};
}}

QPushButton#LadybirdPopoverLink:disabled {{
    color: {3};
}}

QPushButton#LadybirdPopoverRow {{
    background: transparent;
    border: 0;
    border-radius: 6px;
}}

QPushButton#LadybirdPopoverRow:hover,
QToolButton#LadybirdPopoverIconButton:hover {{
    background: {7};
}}

QPushButton#LadybirdPopoverRow:pressed,
QToolButton#LadybirdPopoverIconButton:pressed {{
    background: {8};
}}

QToolButton#LadybirdPopoverIconButton {{
    background: transparent;
    border: 0;
    border-radius: 6px;
}}
)",
        style_sheet_color(panel.text), style_sheet_color(panel.surface), style_sheet_color(over(panel.edge, panel.surface)), style_sheet_color(panel.muted),
        style_sheet_color(panel.separator), style_sheet_color(panel.link), style_sheet_color(panel.warning), style_sheet_color(panel.control), style_sheet_color(panel.pressed));
}

QString all_tabs_popover_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    return qformatted(R"(
QFrame#LadybirdAllTabsPopover {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-radius: 12px;
}}

QLineEdit#LadybirdAllTabsSearch {{
    color: {0};
    background: {3};
    border: 0;
    border-radius: 7px;
    min-height: 28px;
    max-height: 28px;
    padding: 0 6px 0 2px;
    font-size: 13px;
    selection-background-color: {4};
}}

QScrollArea#LadybirdAllTabsList, QScrollArea#LadybirdAllTabsList > QWidget {{
    background: transparent;
}}

QLabel#LadybirdAllTabsHeading {{
    color: {5};
    background: transparent;
    font-size: 12px;
}}

QFrame#LadybirdPopoverSeparator {{
    background: {6};
    border: 0;
}}
)",
        style_sheet_color(panel.text), style_sheet_color(panel.surface), style_sheet_color(over(panel.edge, panel.surface)), style_sheet_color(panel.control),
        style_sheet_color(Theme::selection()), style_sheet_color(panel.muted), style_sheet_color(panel.separator));
}

QString zoom_popover_style_sheet(QPalette const&)
{
    auto const& panel = Theme::panel();
    return qformatted(R"(
QFrame#LadybirdZoomPopover {{
    color: {0};
    background: {1};
    border: 1px solid {2};
    border-radius: 12px;
}}

QLabel#LadybirdZoomLevel {{
    color: {0};
    font-size: 15px;
    font-weight: 600;
}}

QPushButton#LadybirdZoomButton {{
    background: {3};
    border: 0;
    border-radius: 7px;
}}

QPushButton#LadybirdZoomButton:pressed {{
    background: {4};
}}

QPushButton#LadybirdPopoverLink {{
    color: {5};
    background: transparent;
    border: 0;
    border-radius: 5px;
    padding: 4px 6px;
    font-size: 12px;
}}

QPushButton#LadybirdPopoverLink:hover {{
    background: {3};
}}
)",
        style_sheet_color(panel.text), style_sheet_color(panel.surface), style_sheet_color(over(panel.edge, panel.surface)),
        style_sheet_color(panel.control), style_sheet_color(panel.pressed), style_sheet_color(panel.link));
}

// Tooltips appear after half a second of hover. Once one has shown, the ones next to it appear at once for 1.5 s.
static constexpr int TOOLTIP_DELAY_MS = 500;
static constexpr int TOOLTIP_GRACE_PERIOD_MS = 1500;
// Submenus open after 150 ms of hover, and stay open only while the pointer moves toward them: the first move in another
// direction selects whatever the pointer is over.
static constexpr int SUBMENU_DELAY_MS = 150;

// A menu's rows are 26 px tall, their text inset 10 px from each side of the row. Shortcuts and submenu chevrons end at
// that same inset on the right, so the menu has one right edge.
static constexpr int MENU_PADDING = 6;
static constexpr int MENU_RADIUS = 10;
static constexpr int MENU_ROW_HEIGHT = 26;
static constexpr int MENU_ROW_INSET = 10;
static constexpr int MENU_ROW_RADIUS = 6;
static constexpr int MENU_SEPARATOR_HEIGHT = 11;
static constexpr int MENU_SEPARATOR_INSET = 8;
static constexpr int MENU_ICON_SIZE = 16;
static constexpr int MENU_ICON_GAP = 6;
static constexpr int MENU_SHORTCUT_GAP = 24;
static constexpr int MENU_CHEVRON_SIZE = 12;

static bool menu_item_has_leading_column(QStyleOptionMenuItem const& item)
{
    return item.maxIconWidth > 0 || item.menuHasCheckableItems;
}

static void paint_menu_item(QStyleOptionMenuItem const& item, QPainter& painter, bool underlines_mnemonics)
{
    auto const& panel = Theme::panel();
    auto rect = item.rect;

    if (item.menuItemType == QStyleOptionMenuItem::Separator) {
        auto y = rect.top() + (rect.height() / 2);
        painter.fillRect(QRect(rect.left() + MENU_SEPARATOR_INSET, y, rect.width() - (2 * MENU_SEPARATOR_INSET), 1), panel.separator);
        return;
    }

    auto enabled = item.state.testFlag(QStyle::State_Enabled);
    auto selected = enabled && item.state.testFlag(QStyle::State_Selected);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (selected) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(Theme::selected_row());
        painter.drawRoundedRect(QRectF(rect), MENU_ROW_RADIUS, MENU_ROW_RADIUS);
    }

    auto text_color = !enabled ? Theme::chrome().disabled : (selected ? QColor(Qt::white) : panel.text);
    auto secondary_color = !enabled ? Theme::chrome().disabled : (selected ? QColor(Qt::white) : panel.shortcut);

    auto content = rect.adjusted(MENU_ROW_INSET, 0, -MENU_ROW_INSET, 0);

    // A check mark or an icon leads the row, in a column the rows of the menu share.
    if (menu_item_has_leading_column(item)) {
        QRect leading { content.left(), content.top() + ((content.height() - MENU_ICON_SIZE) / 2), MENU_ICON_SIZE, MENU_ICON_SIZE };
        if (item.checkType != QStyleOptionMenuItem::NotCheckable && item.checked) {
            QPainterPath check;
            check.moveTo(3.5, 8.5);
            check.lineTo(6.5, 11.5);
            check.lineTo(12.5, 4.5);
            painter.save();
            painter.translate(leading.topLeft());
            painter.setPen(QPen(text_color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(check);
            painter.restore();
        } else if (!item.icon.isNull()) {
            item.icon.paint(&painter, leading, Qt::AlignCenter, enabled ? (selected ? QIcon::Selected : QIcon::Normal) : QIcon::Disabled);
        }
        content.setLeft(leading.right() + 1 + MENU_ICON_GAP);
    }

    if (item.menuItemType == QStyleOptionMenuItem::SubMenu) {
        QRect chevron { content.right() + 1 - MENU_CHEVRON_SIZE, content.top() + ((content.height() - MENU_CHEVRON_SIZE) / 2), MENU_CHEVRON_SIZE, MENU_CHEVRON_SIZE };
        QPainterPath path;
        path.moveTo(6, 3.5);
        path.lineTo(10.5, 8);
        path.lineTo(6, 12.5);
        painter.save();
        painter.translate(chevron.topLeft());
        painter.scale(MENU_CHEVRON_SIZE / 16.0, MENU_CHEVRON_SIZE / 16.0);
        painter.setPen(QPen(secondary_color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        painter.restore();
        content.setRight(chevron.left() - MENU_SHORTCUT_GAP);
    }

    auto text_flags = Qt::AlignVCenter | Qt::TextSingleLine | (underlines_mnemonics ? Qt::TextShowMnemonic : Qt::TextHideMnemonic);
    auto label = item.text;
    if (auto tab = label.indexOf(u'\t'); tab >= 0) {
        painter.setPen(secondary_color);
        painter.setFont(item.font);
        painter.drawText(content, text_flags | Qt::AlignRight, label.mid(tab + 1));
        label = label.left(tab);
    }
    painter.setPen(text_color);
    painter.setFont(item.font);
    painter.drawText(content, text_flags | Qt::AlignLeft, label);
    painter.restore();
}

class ApplicationStyle final : public QProxyStyle {
public:
    AK_ALLOC_WITH_KMALLOC;

    // A menu is a rounded card with a hairline edge, which needs a window it can be cut out of.
    virtual void polish(QWidget* widget) override
    {
        QProxyStyle::polish(widget);
        if (qobject_cast<QMenu*>(widget))
            widget->setAttribute(Qt::WA_TranslucentBackground);
    }

    virtual void drawPrimitive(PrimitiveElement element, QStyleOption const* option, QPainter* painter, QWidget const* widget) const override
    {
        if (element == PE_PanelMenu) {
            auto const& panel = Theme::panel();
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(QPen(over(panel.edge, panel.surface), 1));
            painter->setBrush(panel.surface);
            painter->drawRoundedRect(QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5), MENU_RADIUS - 0.5, MENU_RADIUS - 0.5);
            painter->restore();
            return;
        }
        if (element == PE_FrameMenu)
            return;
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    virtual int pixelMetric(PixelMetric metric, QStyleOption const* option, QWidget const* widget) const override
    {
        if (metric == PM_MenuPanelWidth)
            return 1;
        if (metric == PM_MenuHMargin || metric == PM_MenuVMargin)
            return MENU_PADDING - 1;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    virtual void drawControl(ControlElement element, QStyleOption const* option, QPainter* painter, QWidget const* widget) const override
    {
        if (element == CE_MenuItem) {
            if (auto const* item = qstyleoption_cast<QStyleOptionMenuItem const*>(option)) {
                paint_menu_item(*item, *painter, styleHint(SH_UnderlineShortcut, option, widget, nullptr) != 0);
                return;
            }
        }
        if (element == CE_MenuEmptyArea)
            return;
        QProxyStyle::drawControl(element, option, painter, widget);
    }

    virtual QSize sizeFromContents(ContentsType type, QStyleOption const* option, QSize const& contents_size, QWidget const* widget) const override
    {
        if (type == CT_MenuItem) {
            if (auto const* item = qstyleoption_cast<QStyleOptionMenuItem const*>(option)) {
                if (item->menuItemType == QStyleOptionMenuItem::Separator)
                    return { contents_size.width(), MENU_SEPARATOR_HEIGHT };
                // NB: Qt measures the label and the shortcut column into the contents' width.
                auto width = contents_size.width() + (2 * MENU_ROW_INSET) + MENU_SHORTCUT_GAP;
                if (menu_item_has_leading_column(*item))
                    width += MENU_ICON_SIZE + MENU_ICON_GAP;
                if (item->menuItemType == QStyleOptionMenuItem::SubMenu)
                    width += MENU_CHEVRON_SIZE + MENU_SHORTCUT_GAP;
                return { width, MENU_ROW_HEIGHT };
            }
        }
        return QProxyStyle::sizeFromContents(type, option, contents_size, widget);
    }

    virtual int styleHint(StyleHint hint, QStyleOption const* option, QWidget const* widget, QStyleHintReturn* return_data) const override
    {
        if (hint == SH_ToolTip_WakeUpDelay)
            return TOOLTIP_DELAY_MS;
        if (hint == SH_ToolTip_FallAsleepDelay)
            return TOOLTIP_GRACE_PERIOD_MS;
        if (hint == SH_Menu_SubMenuPopupDelay || hint == SH_Menu_SubMenuSloppyCloseTimeout)
            return SUBMENU_DELAY_MS;
        if (hint == SH_Menu_SubMenuUniDirection)
            return true;
        if (hint == SH_Menu_SubMenuUniDirectionFailCount)
            return 1;
        return QProxyStyle::styleHint(hint, option, widget, return_data);
    }
};

void install_application_style()
{
    QApplication::setStyle(new ApplicationStyle);
}

QString primary_button_style_sheet()
{
    auto primary = Theme::primary();
    return qformatted(R"(
QPushButton {{
    color: #ffffff;
    background: {0};
    border: 0;
    border-radius: 8px;
    min-height: 32px;
    padding: 0 16px;
    font-weight: 500;
}}

QPushButton:hover {{
    background: {1};
}}

QPushButton:pressed {{
    background: {2};
}}
)",
        style_sheet_color(primary), style_sheet_color(primary.darker(110)), style_sheet_color(primary.darker(122)));
}

QString restore_session_card_style_sheet()
{
    auto const& panel = Theme::panel();
    auto primary = Theme::primary();
    return qformatted(R"(
QFrame#LadybirdRestoreSessionCard {{
    color: {0};
    background: {1};
    border: 0;
    border-radius: 12px;
}}

QFrame#LadybirdRestoreSessionCard QLabel {{
    background: transparent;
    font-size: 13px;
}}

QLabel#LadybirdRestoreSessionTitle {{
    color: {0};
    font-weight: 600;
}}

QLabel#LadybirdRestoreSessionQuestion {{
    color: {2};
}}

QFrame#LadybirdRestoreSessionCard QPushButton {{
    border: 0;
    border-radius: 7px;
    min-height: 28px;
    padding: 0 14px;
    font-size: 13px;
}}

QPushButton#LadybirdRestoreSessionNotNow {{
    color: {0};
    background: {3};
}}

QPushButton#LadybirdRestoreSessionNotNow:pressed {{
    background: {4};
}}

QPushButton#LadybirdRestoreSessionRestore {{
    color: #ffffff;
    background: {5};
    font-weight: 500;
}}

QPushButton#LadybirdRestoreSessionRestore:hover {{
    background: {6};
}}

QPushButton#LadybirdRestoreSessionRestore:pressed {{
    background: {7};
}}
)",
        style_sheet_color(panel.text), style_sheet_color(panel.surface), style_sheet_color(panel.muted), style_sheet_color(panel.control), style_sheet_color(panel.pressed),
        style_sheet_color(primary), style_sheet_color(primary.darker(110)), style_sheet_color(primary.darker(122)));
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

Optional<ControlFrameColors> control_frame_colors(QAbstractButton const& button, ControlFrameStyle frame_style)
{
    if (!button.isEnabled())
        return {};

    auto pressed = button.isDown() || button.isChecked();
    // NB: Qt keeps a button hovered until the pointer moves, even after a relayout has moved the button away from the
    //     pointer, as when a button is clicked and its toolbar or sidebar changes around it. So also check that the
    //     pointer is actually over the button.
    auto hovered = button.underMouse() && button.rect().contains(button.mapFromGlobal(QCursor::pos()));

    if (frame_style == ControlFrameStyle::InteractionOnly) {
        // A frameless button can sit on the chrome or on a lighter tab, so its fills are translucent and deepen whatever
        // is below.
        if (pressed)
            return ControlFrameColors { Theme::panel().pressed, Qt::transparent };
        if (hovered)
            return ControlFrameColors { Theme::chrome(button.palette()).hover, Qt::transparent };
        return {};
    }

    auto const& palette = button.window()->palette();
    auto background = chrome_active_tab_surface(palette);
    auto border = chrome_control_surface_hover(palette);
    if (hovered) {
        background = chrome_control_surface_hover(palette);
        border = chrome_control_border(palette);
    }
    if (pressed) {
        background = chrome_control_surface_pressed(palette);
        border = chrome_control_border(palette);
    }
    return ControlFrameColors { background, border };
}

// Keyboard focus is ringed with a fine line of the focus edge color right outside the control, in a wider, fainter halo.
void paint_focus_ring(QPainter& painter, QRectF const& rect, qreal radius)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Theme::focus_ring(), 2));
    painter.drawRoundedRect(rect.adjusted(-2, -2, 2, 2), radius + 2, radius + 2);
    painter.setPen(QPen(Theme::focus_edge(), 1));
    painter.drawRoundedRect(rect.adjusted(-0.5, -0.5, 0.5, 0.5), radius + 0.5, radius + 0.5);
    painter.restore();
}

void paint_control_frame(QPainter& painter, QAbstractButton const& button, ControlFrameStyle frame_style)
{
    auto colors = control_frame_colors(button, frame_style);
    if (!colors.has_value())
        return;

    // Buttons on the chrome are rounded squares. The small buttons that sit over a collapsed sidebar's tabs are circles.
    if (frame_style == ControlFrameStyle::ActiveTabOverlay) {
        // NB: The badge sits on a favicon, so it is ringed with a hairline to keep it apart from what it covers.
        auto ring = colors->border;
        if (!button.isDown() && !button.isChecked() && !(button.underMouse() && button.rect().contains(button.mapFromGlobal(QCursor::pos()))))
            ring = Theme::chrome(button.window()->palette()).separator;
        paint_circular_control_frame(painter, button.rect(), button.devicePixelRatioF(), colors->background, ring);
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors->background);
    painter.drawRoundedRect(QRectF(button.rect()), CONTROL_FRAME_RADIUS, CONTROL_FRAME_RADIUS);
    painter.restore();
}

// A ring around a favicon, with an arc in the accent color going around it, to show that the page is loading. Every
// ring follows the same clock, so the rings on screen turn in step.
// NB: The radii are to the middle of the 1.5 px line, so the ring's outer edge is 0.75 px further out.
static constexpr qreal LOADING_RING_LINE_WIDTH = 1.5;
static constexpr qreal FAVICON_LOADING_RING_RADIUS = 9.25;
static constexpr qreal TOOLBAR_BUTTON_LOADING_RING_RADIUS = 10.75;

static QRectF loading_ring_rect(QPointF center, LoadingRingSize size)
{
    auto radius = size == LoadingRingSize::Favicon ? FAVICON_LOADING_RING_RADIUS : TOOLBAR_BUTTON_LOADING_RING_RADIUS;
    return { center.x() - radius, center.y() - radius, 2 * radius, 2 * radius };
}

void paint_progress_ring(QPainter& painter, QPalette const& palette, QPointF center, double progress)
{
    auto ring_rect = loading_ring_rect(center, LoadingRingSize::ToolbarButton);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Theme::chrome(palette).separator, LOADING_RING_LINE_WIDTH));
    painter.drawEllipse(ring_rect);
    // NB: Qt's angles are in sixteenths of a degree, counterclockwise from three o'clock.
    painter.setPen(QPen(Theme::accent(), LOADING_RING_LINE_WIDTH, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(ring_rect, 90 * 16, -qRound(clamp(progress, 0.0, 1.0) * 360 * 16));
    painter.restore();
}

void paint_loading_ring(QPainter& painter, QPalette const& palette, QPointF center, LoadingRingSize size, LoadingPhase loading_phase)
{
    static QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();
    auto period = loading_phase == LoadingPhase::WaitingForResponse ? LOADING_RING_WAITING_PERIOD_MS : LOADING_RING_PERIOD_MS;
    auto phase = static_cast<qreal>(clock.elapsed() % period) / period;

    auto ring_rect = loading_ring_rect(center, size);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    // NB: With reduced motion, the ring pulses its opacity instead of turning, at full strength in both phases.
    if (loading_phase == LoadingPhase::WaitingForResponse && !SystemAppearance::prefers_reduced_motion())
        painter.setOpacity(painter.opacity() * 0.5);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Theme::chrome(palette).separator, LOADING_RING_LINE_WIDTH));
    painter.drawEllipse(ring_rect);

    // NB: Qt's angles are in sixteenths of a degree, counterclockwise from three o'clock. The arc is a quarter of the ring
    //     and turns clockwise, starting from twelve o'clock. With reduced motion, it stays at twelve o'clock and pulses.
    auto reduce_motion = SystemAppearance::prefers_reduced_motion();
    auto start_angle = reduce_motion ? 90 * 16 : qRound((90.0 - (phase * 360.0)) * 16);
    if (reduce_motion)
        painter.setOpacity(painter.opacity() * (0.35 + (0.65 * (0.5 + (0.5 * qCos(phase * 2 * M_PI))))));
    painter.setPen(QPen(Theme::accent(), LOADING_RING_LINE_WIDTH, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(ring_rect, start_angle, -90 * 16);
    painter.restore();
}

}
