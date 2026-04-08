/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <adwaita.h>

struct LadybirdBrowserWindow;

namespace LadybirdWidgets {

LadybirdBrowserWindow* create_browser_window_widget(AdwApplication* app);

AdwHeaderBar* browser_window_header_bar(LadybirdBrowserWindow*);
AdwTabView* browser_window_tab_view(LadybirdBrowserWindow*);
GtkButton* browser_window_restore_button(LadybirdBrowserWindow*);
GtkLabel* browser_window_zoom_label(LadybirdBrowserWindow*);
AdwBanner* browser_window_devtools_banner(LadybirdBrowserWindow*);
GtkRevealer* browser_window_find_bar_revealer(LadybirdBrowserWindow*);
GtkSearchEntry* browser_window_find_entry(LadybirdBrowserWindow*);
GtkLabel* browser_window_find_result_label(LadybirdBrowserWindow*);
GMenu* browser_window_hamburger_menu(LadybirdBrowserWindow*);
AdwToastOverlay* browser_window_toast_overlay(LadybirdBrowserWindow*);
GtkMenuButton* browser_window_menu_button(LadybirdBrowserWindow*);

}
