/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/Cursor.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/SelectItem.h>
#include <LibWebView/Menu.h>
#include <LibWebView/URL.h>
#include <UI/Gtk/BrowserWindow.h>
#include <UI/Gtk/Dialogs.h>
#include <UI/Gtk/Events.h>
#include <UI/Gtk/GLibPtr.h>
#include <UI/Gtk/Menu.h>
#include <UI/Gtk/Tab.h>
#include <UI/Gtk/WebContentView.h>
#include <UI/Gtk/Widgets/Builder.h>

namespace Ladybird {

struct ListPopoverShell {
    GtkPopover* popover { nullptr };
    GtkListBox* list_box { nullptr };
};

static ListPopoverShell create_list_popover_shell(GtkWidget* parent)
{
    GObjectPtr builder { gtk_builder_new_from_resource("/org/ladybird/Ladybird/gtk/list-popover.ui") };
    auto* popover = LadybirdWidgets::get_builder_object<GtkPopover>(builder, "popover");
    auto* list_box = LadybirdWidgets::get_builder_object<GtkListBox>(builder, "list_box");
    gtk_widget_set_parent(GTK_WIDGET(popover), parent);
    return { popover, list_box };
}

static void append_select_option_row(GtkListBox* list_box, char const* label, bool selected, bool disabled, unsigned id, int margin_start)
{
    auto* row = gtk_list_box_row_new();
    auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, margin_start);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 4);
    gtk_widget_set_margin_bottom(box, 4);

    if (selected) {
        auto* check = gtk_image_new_from_icon_name("object-select-symbolic");
        gtk_box_append(GTK_BOX(box), check);
    } else {
        auto* spacer = gtk_image_new_from_icon_name("object-select-symbolic");
        gtk_widget_set_opacity(spacer, 0);
        gtk_box_append(GTK_BOX(box), spacer);
    }

    auto* label_widget = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0.0);
    gtk_box_append(GTK_BOX(box), label_widget);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_widget_set_sensitive(row, !disabled);
    g_object_set_data(G_OBJECT(row), "item-id", GUINT_TO_POINTER(id));
    gtk_list_box_append(GTK_LIST_BOX(list_box), row);
}

Tab::Tab(BrowserWindow& window, URL::URL url)
    : Tab(window, nullptr, 0)
{
    m_initial_url = move(url);
    if (!m_initial_url.scheme().is_empty())
        navigate(m_initial_url);
}

Tab::Tab(BrowserWindow& window, WebView::WebContentClient& parent_client, u64 page_index)
    : Tab(window, &parent_client, page_index)
{
}

Tab::Tab(BrowserWindow& window, RefPtr<WebView::WebContentClient> parent_client, size_t page_index)
    : m_window(window)
{
    m_web_view = ladybird_web_view_new();
    gtk_widget_set_vexpand(GTK_WIDGET(m_web_view), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(m_web_view), TRUE);
    m_view = adopt_own(*new WebContentView(m_web_view, parent_client, page_index));

    setup_callbacks();
}

Tab::~Tab() = default;

void Tab::setup_callbacks()
{
    auto* root = GTK_WIDGET(m_web_view);

    m_view->on_title_change = [this](auto const& title) {
        if (m_tab_page) {
            auto utf8 = title.to_utf8();
            auto byte_str = ByteString(utf8.bytes_as_string_view());
            adw_tab_page_set_title(m_tab_page, byte_str.characters());
        }
    };

    m_view->on_url_change = [this](auto const& url) {
        if (m_window.current_tab() != this)
            return;
        if (BrowserWindow::is_internal_url(url)) {
            m_window.update_location_entry(""sv);
            return;
        }
        auto url_string = url.serialize();
        m_window.update_location_entry(url_string.bytes_as_string_view());
    };

    m_view->on_load_start = [this](auto const&, bool) {
        if (m_tab_page)
            adw_tab_page_set_loading(m_tab_page, TRUE);
    };

    m_view->on_load_finish = [this](auto const&) {
        if (m_tab_page)
            adw_tab_page_set_loading(m_tab_page, FALSE);
    };

    m_view->on_cursor_change = [root](auto const& cursor) {
        auto cursor_name = cursor.visit(
            [](Gfx::StandardCursor standard_cursor) -> StringView {
                return standard_cursor_to_css_name(standard_cursor);
            },
            [](Gfx::ImageCursor const&) -> StringView {
                return "default"sv;
            });
        auto cursor_name_str = ByteString(cursor_name);
        GObjectPtr gdk_cursor { gdk_cursor_new_from_name(cursor_name_str.characters(), nullptr) };
        gtk_widget_set_cursor(root, GDK_CURSOR(gdk_cursor.ptr()));
    };

    m_view->on_enter_tooltip_area = [root](auto const& tooltip) {
        auto text = ByteString(tooltip);
        gtk_widget_set_tooltip_text(root, text.characters());
    };

    m_view->on_leave_tooltip_area = [root]() {
        gtk_widget_set_tooltip_text(root, nullptr);
    };

    m_view->on_link_hover = [root](auto const& url) {
        auto url_string = url.serialize();
        auto byte_string = ByteString(url_string.bytes_as_string_view());
        gtk_widget_set_tooltip_text(root, byte_string.characters());
    };

    m_view->on_link_unhover = [root]() {
        gtk_widget_set_tooltip_text(root, nullptr);
    };

    m_view->on_new_web_view = [this](auto activate_tab, auto, auto page_index) -> String {
        if (page_index.has_value()) {
            auto& new_tab = m_window.create_child_tab(activate_tab, *this, page_index.value());
            return new_tab.view().handle();
        }
        auto& new_tab = m_window.create_new_tab(activate_tab);
        return new_tab.view().handle();
    };

    m_view->on_activate_tab = [root]() {
        gtk_widget_grab_focus(root);
    };

    m_view->on_close = [this]() {
        m_window.close_tab(*this);
    };

    m_view->on_zoom_level_changed = [this]() {
        m_window.update_zoom_label();
    };

    // Dialogs
    m_view->on_request_alert = [this](auto const& message) {
        Dialogs::show_alert(m_window.gtk_window(), m_view.ptr(), message);
    };

    m_view->on_request_confirm = [this](auto const& message) {
        Dialogs::show_confirm(m_window.gtk_window(), m_view.ptr(), message);
    };

    m_view->on_request_prompt = [this](auto const& message, auto const& default_value) {
        Dialogs::show_prompt(m_window.gtk_window(), m_view.ptr(), message, default_value);
    };

    m_view->on_request_color_picker = [this](Color current_color) {
        Dialogs::show_color_picker(m_window.gtk_window(), m_view.ptr(), current_color);
    };

    m_view->on_request_file_picker = [this](auto const& accepted_file_types, auto allow_multiple) {
        Dialogs::show_file_picker(m_window.gtk_window(), m_view.ptr(), accepted_file_types, allow_multiple);
    };

    m_view->on_request_select_dropdown = [this](Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) {
        show_select_dropdown(content_position, minimum_width, move(items));
    };

    m_view->on_find_in_page = [this](auto current_match_index, auto const& total_match_count) {
        m_window.update_find_in_page_result(current_match_index, total_match_count);
    };

    m_view->on_fullscreen_window = [this]() {
        gtk_window_fullscreen(m_window.gtk_window());
    };

    m_view->on_exit_fullscreen_window = [this]() {
        gtk_window_unfullscreen(m_window.gtk_window());
    };

    m_view->on_restore_window = [this]() {
        gtk_window_unmaximize(m_window.gtk_window());
        gtk_window_unfullscreen(m_window.gtk_window());
    };

    m_view->on_maximize_window = [this]() {
        gtk_window_maximize(m_window.gtk_window());
    };

    m_view->on_minimize_window = [this]() {
        gtk_window_minimize(m_window.gtk_window());
    };

    m_view->on_reposition_window = [](auto) {
        // GTK4 removed window repositioning APIs. On Wayland, clients cannot
        // position their own top-level windows. On other backends, GTK4 chose
        // not to expose it for portability.
    };

    m_view->on_resize_window = [this](auto size) {
        gtk_window_set_default_size(m_window.gtk_window(), size.width(), size.height());
    };

    m_view->on_favicon_change = [this](auto const& bitmap) {
        if (!m_tab_page)
            return;
        g_autoptr(GBytes) bytes = g_bytes_new(bitmap.scanline_u8(0), bitmap.size_in_bytes());
        GObjectPtr texture { gdk_memory_texture_new(bitmap.width(), bitmap.height(), GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, bitmap.pitch()) };
        adw_tab_page_set_icon(m_tab_page, G_ICON(texture.ptr()));
    };

    m_view->on_audio_play_state_changed = [this](auto play_state) {
        if (m_tab_page) {
            adw_tab_page_set_indicator_icon(m_tab_page,
                play_state == Web::HTML::AudioPlayState::Playing
                    ? g_themed_icon_new("audio-volume-high-symbolic")
                    : nullptr);
        }
    };

    // FIXME: Support non-modal JS dialogs (on_request_set_prompt_text,
    //        on_request_accept_dialog, on_request_dismiss_dialog) for WebDriver support.

    // Context menus
    create_context_menu(*root, *m_view, m_view->page_context_menu());
    create_context_menu(*root, *m_view, m_view->link_context_menu());
    create_context_menu(*root, *m_view, m_view->image_context_menu());
    create_context_menu(*root, *m_view, m_view->media_context_menu());
}

void Tab::navigate(URL::URL const& url)
{
    m_view->load(url);
}

void Tab::show_select_dropdown(Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items)
{
    auto* root = GTK_WIDGET(m_web_view);
    auto shell = create_list_popover_shell(root);
    auto* popover = shell.popover;
    auto* list_box = shell.list_box;
    gtk_widget_set_size_request(GTK_WIDGET(popover), minimum_width, -1);

    auto device_pixel_ratio = m_view->device_pixel_ratio();
    GdkRectangle rect = {
        static_cast<int>(content_position.x() / device_pixel_ratio),
        static_cast<int>(content_position.y() / device_pixel_ratio),
        1, 1
    };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_box), GTK_SELECTION_NONE);

    for (auto const& item : items) {
        item.visit(
            [&](Web::HTML::SelectItemOption const& option) {
                append_select_option_row(list_box, option.label.to_byte_string().characters(), option.selected, option.disabled, option.id, 8);
            },
            [&](Web::HTML::SelectItemOptionGroup const& group) {
                auto* header = gtk_label_new(group.label.to_byte_string().characters());
                gtk_label_set_xalign(GTK_LABEL(header), 0.0);
                gtk_widget_add_css_class(header, "heading");
                gtk_widget_set_margin_start(header, 8);
                gtk_widget_set_margin_top(header, 6);
                gtk_widget_set_margin_bottom(header, 2);
                auto* header_row = gtk_list_box_row_new();
                gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(header_row), header);
                gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(header_row), FALSE);
                gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(header_row), FALSE);
                gtk_list_box_append(GTK_LIST_BOX(list_box), header_row);

                for (auto const& option : group.items) {
                    append_select_option_row(list_box, option.label.to_byte_string().characters(), option.selected, option.disabled, option.id, 16);
                }
            },
            [&](Web::HTML::SelectItemSeparator const&) {
                auto* sep_row = gtk_list_box_row_new();
                gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(sep_row), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
                gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(sep_row), FALSE);
                gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(sep_row), FALSE);
                gtk_list_box_append(GTK_LIST_BOX(list_box), sep_row);
            },
            [&](auto const&) {});
    }

    struct DropdownState {
        WebContentView* view;
        GtkPopover* popover;
        bool selected { false };
    };
    auto* dropdown_state = new DropdownState { m_view.ptr(), GTK_POPOVER(popover) };

    g_signal_connect(list_box, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer user_data) {
        auto* state = static_cast<DropdownState*>(user_data);
        auto item_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "item-id"));
        state->selected = true;
        state->view->select_dropdown_closed(item_id);
        gtk_popover_popdown(state->popover);
    }),
        dropdown_state);

    g_signal_connect(popover, "closed", G_CALLBACK(+[](GtkPopover* popover, gpointer user_data) {
        auto* state = static_cast<DropdownState*>(user_data);
        if (!state->selected)
            state->view->select_dropdown_closed({});
        delete state;
        gtk_widget_unparent(GTK_WIDGET(popover));
    }),
        dropdown_state);

    gtk_popover_popup(GTK_POPOVER(popover));
}

}
