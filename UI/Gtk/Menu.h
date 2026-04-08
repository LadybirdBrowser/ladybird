/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWebView/Menu.h>

#include <gio/gio.h>
#include <gtk/gtk.h>

namespace Ladybird {

class WebContentView;

void add_action_to_map(GActionMap* action_map, char const* action_name, WebView::Action& action, bool observe_state = true);
GMenu* create_application_menu(WebView::Menu& menu, Function<ByteString(WebView::Action&)> const& detailed_action_name_for_action);
void create_context_menu(GtkWidget& parent, WebContentView& view, WebView::Menu& menu);
void add_menu_actions_to_map(GActionMap* action_map, WebView::Menu& menu, Function<ByteString(WebView::Action&)> const& action_name_for_action);
void install_action_accelerators(GtkApplication* application, char const* detailed_action_name, WebView::Action const& action);
void install_menu_action_accelerators(GtkApplication* application, char const* prefix, WebView::Menu& menu);
void append_submenu_to_section_containing_action(GMenu* menu, char const* detailed_action_name, char const* submenu_label, GMenuModel* submenu_model);

}
