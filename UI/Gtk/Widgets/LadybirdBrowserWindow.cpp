/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <UI/Gtk/Widgets/LadybirdBrowserWindow.h>

struct LadybirdBrowserWindow {
    AdwApplicationWindow parent_instance;
    AdwToolbarView* toolbar_view { nullptr };
    AdwHeaderBar* header_bar { nullptr };
    GtkButton* back_button { nullptr };
    GtkButton* forward_button { nullptr };
    GtkButton* reload_button { nullptr };
    GtkButton* restore_button { nullptr };
    GtkMenuButton* menu_button { nullptr };
    AdwTabView* tab_view { nullptr };
    AdwTabBar* tab_bar { nullptr };
    AdwToastOverlay* toast_overlay { nullptr };
    GtkRevealer* find_bar_revealer { nullptr };
    GtkSearchEntry* find_entry { nullptr };
    GtkLabel* find_result_label { nullptr };
    GtkButton* zoom_reset_button { nullptr };
    GtkLabel* zoom_label { nullptr };
    AdwBanner* devtools_banner { nullptr };
    GMenu* hamburger_menu { nullptr };
};

struct LadybirdBrowserWindowClass {
    AdwApplicationWindowClass parent_class;
};

GType ladybird_browser_window_get_type(void);
G_DEFINE_FINAL_TYPE(LadybirdBrowserWindow, ladybird_browser_window, ADW_TYPE_APPLICATION_WINDOW)

static void ladybird_browser_window_class_init(LadybirdBrowserWindowClass* klass)
{
    auto* widget_class = GTK_WIDGET_CLASS(klass);
    gtk_widget_class_set_template_from_resource(widget_class, "/org/ladybird/Ladybird/gtk/browser-window.ui");

    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, toolbar_view);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, header_bar);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, back_button);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, forward_button);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, reload_button);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, restore_button);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, menu_button);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, tab_view);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, tab_bar);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, toast_overlay);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, find_bar_revealer);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, find_entry);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, find_result_label);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, zoom_reset_button);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, zoom_label);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, devtools_banner);
    gtk_widget_class_bind_template_child(widget_class, LadybirdBrowserWindow, hamburger_menu);
}

static void ladybird_browser_window_init(LadybirdBrowserWindow* self)
{
    gtk_widget_init_template(GTK_WIDGET(self));
}

namespace LadybirdWidgets {

LadybirdBrowserWindow* create_browser_window_widget(AdwApplication* app)
{
    return reinterpret_cast<LadybirdBrowserWindow*>(g_object_new(ladybird_browser_window_get_type(),
        "application", app,
        nullptr));
}

AdwHeaderBar* browser_window_header_bar(LadybirdBrowserWindow* window) { return window->header_bar; }
AdwTabView* browser_window_tab_view(LadybirdBrowserWindow* window) { return window->tab_view; }
GtkButton* browser_window_restore_button(LadybirdBrowserWindow* window) { return window->restore_button; }
GtkLabel* browser_window_zoom_label(LadybirdBrowserWindow* window) { return window->zoom_label; }
AdwBanner* browser_window_devtools_banner(LadybirdBrowserWindow* window) { return window->devtools_banner; }
GtkRevealer* browser_window_find_bar_revealer(LadybirdBrowserWindow* window) { return window->find_bar_revealer; }
GtkSearchEntry* browser_window_find_entry(LadybirdBrowserWindow* window) { return window->find_entry; }
GtkLabel* browser_window_find_result_label(LadybirdBrowserWindow* window) { return window->find_result_label; }
GMenu* browser_window_hamburger_menu(LadybirdBrowserWindow* window) { return window->hamburger_menu; }
AdwToastOverlay* browser_window_toast_overlay(LadybirdBrowserWindow* window) { return window->toast_overlay; }
GtkMenuButton* browser_window_menu_button(LadybirdBrowserWindow* window) { return window->menu_button; }

}
