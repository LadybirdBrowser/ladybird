/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>

#include <QColor>
#include <QPalette>

class QWidget;

namespace WebView {

enum class ColorTheme : u8;
enum class ThemePreference : u8;

}

namespace Ladybird::Theme {

// The colors of the window chrome: the tab strip, the toolbar and what sits on them.
struct ChromeColors {
    QColor strip;         // Tab strip, sidebar
    QColor toolbar;       // Toolbar, active tab, selected tab in the sidebar
    QColor sidebar;       // Vertical tabs sidebar and rail
    QColor field;         // Address field fill
    QColor field_focused; // Address field fill while editing
    QColor text;          // Titles, URL domain
    QColor muted;         // URL path, secondary text
    QColor tab_title;     // Inactive tab titles
    QColor icon;          // Toolbar icons
    QColor disabled;      // Disabled controls
    QColor hover;         // Hover fills
    QColor strip_hover;   // Hover fill of controls on the tab strip
    QColor row_title;     // Unselected tab titles in the sidebar
    QColor separator;     // Between inactive tabs
    QColor divider;       // Chrome / page edge
    QColor warning;       // Crashed tab, lookalike URL
    QColor warning_fill;  // Not secure pill
    QColor warning_ink;   // Text and icon on the warning fill
    QColor frame;         // Window outline

    // With increased contrast, the address field and the active tab are outlined. Otherwise these are transparent.
    QColor field_edge;
    QColor active_tab_edge;
};

// The colors of menus, popovers, dialogs and the other surfaces that float over the chrome or the page.
struct PanelColors {
    QColor surface;          // Panel background
    QColor text;             // Primary text
    QColor body;             // Body text, icons on controls
    QColor muted;            // Secondary text
    QColor shortcut;         // Keyboard shortcuts in menus
    QColor separator;        // Separator lines
    QColor separator_soft;   // Lines between rows
    QColor control;          // Fill of secondary buttons and fields
    QColor pressed;          // Fill of a pressed row or button
    QColor page;             // The page behind panels, and fields inside them
    QColor edge;             // Panel outline
    QColor link;             // Text buttons and links
    QColor warning;          // Failures and warnings
    QColor badge_background; // Brand badges
    QColor badge_text;       // Brand badges
    QColor information;      // Informational highlights
};

bool is_dark();
bool has_increased_contrast();

// The interface theme decides the appearance, as Qt's color scheme only follows it where the platform theme honors the
// request.
void set_interface_theme(WebView::ThemePreference);

// The chrome and panel colors, and the accent, are derived from the color theme's base colors.
void set_color_theme(WebView::ColorTheme);

ChromeColors const& chrome();

// A private window's chrome is violet-dark in either appearance, so it never looks like a normal window. The chrome
// inside a window marked as private takes its colors from a palette marked as private.
void mark_private_window(QWidget& window);
QPalette chrome_palette(QWidget const&);
bool is_private(QPalette const&);
ChromeColors const& chrome(QPalette const&);
ChromeColors const& chrome(QWidget const&);
bool is_dark(QPalette const&);
bool is_dark(QWidget const&);
PanelColors const& panel();

// The accent color the desktop's settings portal reports, as Qt's palette does not follow it.
void set_desktop_accent(Optional<QColor>);
Optional<QColor> const& desktop_accent();

// The color theme's accent, or the system's accent color if the theme has none, adjusted so it stays clear on the chrome
// in light and dark appearances. It is used only for focus rings, selection and load progress.
QColor accent();
// A focus ring is a fine line of the focus edge color right outside the focused control, in a wider, fainter halo.
QColor focus_edge();
QColor focus_ring();

// Selected text in a field. A color theme tints it with its accent and keeps the text in its normal color, so it stays
// readable on every theme and looks unlike a selected row. A window in the background shows a gray tint instead.
// Without a color theme or a desktop accent, it is the system's own text selection color.
struct TextSelectionColors {
    QColor background;
    QColor text;
};
TextSelectionColors address_field_text_selection(QPalette const&, QPalette::ColorGroup);
TextSelectionColors panel_text_selection(QPalette::ColorGroup, QColor const& text);

// The accent, deep enough that white text on it stays clear.
QColor accent_fill();

// The fill of a primary button, a blue that white text stays clear on.
QColor primary();
// The fill of the selected row of a menu or a list navigated with the keyboard.
QColor selected_row();

// The color of Ladybird's logomark on the chrome: Ladybird purple in the neutral theme, and a tinted theme's base color,
// so the window shows a single hue.
QColor logomark();

}
