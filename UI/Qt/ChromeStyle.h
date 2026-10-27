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
class QPointF;
class QPalette;
class QRectF;
class QWidget;

namespace Ladybird::ChromeStyle {

constexpr inline int LOCATION_EDIT_CORNER_RADIUS = 7;
// One turn of the loading ring.
constexpr inline int LOADING_RING_PERIOD_MS = 900;
// Before the site responds, the ring turns more slowly, at half opacity.
constexpr inline int LOADING_RING_WAITING_PERIOD_MS = 1600;
enum class LoadingRingSize : u8 {
    // 20 px, reaching 2 px past a tab's favicon slot.
    Favicon,
    // 23 px, filling a toolbar button.
    ToolbarButton,
};
enum class LoadingPhase : u8 {
    WaitingForResponse,
    Receiving,
};

bool is_dark(QPalette const&);

// A QGraphicsDropShadowEffect spreads its blur far less than a CSS shadow with the same blur radius does. Measured against
// the design, a blur radius 2.5 times the CSS one matches its shadows.
constexpr qreal drop_shadow_blur_radius(qreal css_blur_radius)
{
    return css_blur_radius * 2.5;
}
QColor mix(QColor const& from, QColor const& to, double amount);
QColor chrome_text(QPalette const&);
QColor chrome_button_text(QPalette const&);
enum class WindowActivity : u8 {
    Active,
    Inactive,
};
WindowActivity window_activity(QWidget const&);

QColor chrome_tab_strip_background(QPalette const&);
QColor chrome_sidebar_background(QPalette const&);
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
QString location_edit_style_sheet(QPalette const&);
QString bookmarks_bar_style_sheet(QPalette const&, int corner_radius);
QString find_in_page_style_sheet(QPalette const&);
// The color of the find bar's previous and next buttons, while there is no match to go to.
QColor find_in_page_unavailable_control_color();
QString javascript_dialog_style_sheet(QPalette const&);
QString bookmark_dialog_style_sheet(QPalette const&);
QString report_review_style_sheet(QPalette const&);
QColor report_review_placeholder_text(QPalette const&);
QString devtools_banner_style_sheet(QPalette const&);
QString tab_widget_style_sheet(QPalette const&);
QString autocomplete_popup_style_sheet(QPalette const&);
QString downloads_popover_style_sheet(QPalette const&);
QString message_popover_style_sheet(QPalette const&);
QString site_settings_popover_style_sheet(QPalette const&);
QString permission_prompt_style_sheet(QPalette const&);
QString all_tabs_popover_style_sheet(QPalette const&);
QString bookmark_popover_style_sheet(QPalette const&);
QString zoom_popover_style_sheet(QPalette const&);
// A button filled with the primary blue, for the main action of a page the browser shows.
QString primary_button_style_sheet();

// Installs the application style, which keeps the platform's look but follows the design's timing.
void install_application_style();
QString restore_session_card_style_sheet();
QString screenshot_selection_style_sheet(QPalette const&);
QString screenshot_viewer_style_sheet();

enum class ControlFrameStyle {
    InteractionOnly,
    ActiveTabOverlay,
};
struct ControlFrameColors {
    QColor background;
    QColor border;
};
// The corner radius of a button's frame on the chrome.
constexpr inline qreal CONTROL_FRAME_RADIUS = 6;
// The colors of a button's frame in the button's current state, or nothing while it shows no frame.
Optional<ControlFrameColors> control_frame_colors(QAbstractButton const&, ControlFrameStyle = ControlFrameStyle::InteractionOnly);
void paint_control_frame(QPainter&, QAbstractButton const&, ControlFrameStyle = ControlFrameStyle::InteractionOnly);
void paint_focus_ring(QPainter&, QRectF const&, qreal radius);

void paint_loading_ring(QPainter&, QPalette const&, QPointF center, LoadingRingSize, LoadingPhase = LoadingPhase::Receiving);
// The loading ring's track, with an arc for the progress made, from 0 to 1, clockwise from twelve o'clock.
void paint_progress_ring(QPainter&, QPalette const&, QPointF center, double progress);

}
