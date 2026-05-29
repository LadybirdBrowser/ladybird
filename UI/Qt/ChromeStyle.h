/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

namespace Ladybird::ChromeStyle {

bool is_dark(QPalette const&);
QColor mix(QColor const& from, QColor const& to, double amount);
QColor chrome_text(QPalette const&);
QColor chrome_button_text(QPalette const&);
QColor chrome_background(QPalette const&);
QColor chrome_surface(QPalette const&);
QColor chrome_surface_recessed(QPalette const&);
QColor chrome_surface_hover(QPalette const&);
QColor chrome_surface_pressed(QPalette const&);
QColor chrome_control_border(QPalette const&);
QColor chrome_active_tab_surface_top(QPalette const&);
QColor chrome_active_tab_surface_bottom(QPalette const&);
QColor chrome_border(QPalette const&);
QColor chrome_accent(QPalette const&);
QColor chrome_muted_text(QPalette const&);

QString style_sheet_color(QColor const&);
QString application_style_sheet(QPalette const&);
QString toolbar_container_style_sheet(QPalette const&);
QString menu_bar_style_sheet(QPalette const&);
QString location_edit_style_sheet(QPalette const&);
QString bookmarks_bar_style_sheet(QPalette const&);
QString find_in_page_style_sheet(QPalette const&);
QString devtools_banner_style_sheet(QPalette const&);
QString tab_widget_style_sheet(QPalette const&);
QString autocomplete_popup_style_sheet(QPalette const&);

}
