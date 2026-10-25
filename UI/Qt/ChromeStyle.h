/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <QColor>
#include <QString>

class QAbstractButton;
class QFont;
class QPainter;
class QPainterPath;
class QPalette;
class QWidget;

namespace Ladybird::ChromeStyle {

constexpr inline int LOCATION_EDIT_CORNER_RADIUS = 16;
constexpr inline int LOADING_SWIRL_PERIOD_MS = 2000;

bool is_dark(QPalette const&);
QColor mix(QColor const& from, QColor const& to, double amount);
QColor chrome_text(QPalette const&);
QColor chrome_button_text(QPalette const&);
enum class WindowActivity : u8 {
    Active,
    Inactive,
};
WindowActivity window_activity(QWidget const&);

QColor chrome_tab_strip_background(QPalette const&, WindowActivity = WindowActivity::Active);
QColor chrome_background(QPalette const&);
QColor chrome_surface(QPalette const&);
QColor chrome_surface_recessed(QPalette const&);
QColor chrome_surface_hover(QPalette const&);
QColor chrome_surface_pressed(QPalette const&);
QColor chrome_control_border(QPalette const&);
QColor chrome_active_tab_surface(QPalette const&);
QColor chrome_border(QPalette const&);
QColor location_edit_border(QPalette const&);
QColor chrome_separator(QPalette const&);
QColor chrome_window_outline(QPalette const&);
QColor chrome_accent();
QColor chrome_muted_text(QPalette const&);
QFont chrome_title_font(QFont);

QString style_sheet_color(QColor const&);
QString application_style_sheet(QPalette const&);
QString toolbar_container_style_sheet(QPalette const&);
QString menu_bar_style_sheet(QPalette const&);
QString hamburger_style_sheet(QPalette const&);
QString location_edit_style_sheet(QPalette const&);
QString bookmarks_bar_style_sheet(QPalette const&, int corner_radius);
QString find_in_page_style_sheet(QPalette const&);
QString javascript_dialog_style_sheet(QPalette const&);
QString bookmark_dialog_style_sheet(QPalette const&);
QString report_review_style_sheet(QPalette const&);
QColor report_review_placeholder_text(QPalette const&);
QString devtools_banner_style_sheet(QPalette const&);
QString tab_widget_style_sheet(QPalette const&);
QString autocomplete_popup_style_sheet(QPalette const&);
QString downloads_popover_style_sheet(QPalette const&);
QString message_popover_style_sheet(QPalette const&);
QString content_blocking_popover_style_sheet(QPalette const&);
QString screenshot_selection_style_sheet(QPalette const&);
QString screenshot_viewer_style_sheet();

enum class CircularControlFrameStyle {
    InteractionOnly,
    ActiveTabOverlay,
};
struct ControlFrameColors {
    QColor background;
    QColor border;
};
// The colors of a button's frame in the button's current state, or nothing while it shows no frame.
Optional<ControlFrameColors> circular_control_frame_colors(QAbstractButton const&, CircularControlFrameStyle = CircularControlFrameStyle::InteractionOnly);
void paint_circular_control_frame(QPainter&, QAbstractButton const&, CircularControlFrameStyle = CircularControlFrameStyle::InteractionOnly);

void paint_loading_swirl(QPainter&, QPainterPath const& border);

}
