/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/URL.h>
#include <UI/Gtk/GLibPtr.h>
#include <UI/Gtk/Widgets/Builder.h>
#include <UI/Gtk/Widgets/LadybirdLocationEntry.h>

struct LocationEntryState {
    NonnullOwnPtr<WebView::Autocomplete> autocomplete;
    Vector<WebView::AutocompleteSuggestion> suggestions;
    int selected_index { -1 };
    String user_text;
    bool is_focused { false };
    bool updating_text { false };
    Function<void(String)> on_navigate;
};

#define LADYBIRD_LOCATION_ENTRY(obj) (reinterpret_cast<LadybirdLocationEntry*>(obj))
#define LADYBIRD_TYPE_LOCATION_ENTRY (ladybird_location_entry_get_type())

struct LadybirdLocationEntry {
    GtkEntry parent_instance;

    GtkPopover* popover { nullptr };
    GtkListBox* list_box { nullptr };
    // GObject allocates this struct with g_malloc0, which zero-fills without
    // calling C++ constructors. OwnPtr is safe here because zero-initialized
    // OwnPtr is equivalent to nullptr (empty state).
    OwnPtr<LocationEntryState> state;
};

struct LadybirdLocationEntryClass {
    GtkEntryClass parent_class;
};

G_DEFINE_FINAL_TYPE(LadybirdLocationEntry, ladybird_location_entry, GTK_TYPE_ENTRY)

static void ladybird_location_entry_update_display_attributes(LadybirdLocationEntry* self);
static void ladybird_location_entry_show_completions(LadybirdLocationEntry* self);
static void ladybird_location_entry_hide_completions(LadybirdLocationEntry* self);
static void ladybird_location_entry_navigate(LadybirdLocationEntry* self);
static void ladybird_location_entry_move_selection(LadybirdLocationEntry* self, int delta);
static void ladybird_location_entry_apply_selected_suggestion(LadybirdLocationEntry* self);

static void set_entry_text_suppressed(LadybirdLocationEntry* self, char const* text, bool move_cursor_to_end = false)
{
    self->state->updating_text = true;
    gtk_editable_set_text(GTK_EDITABLE(self), text);
    if (move_cursor_to_end)
        gtk_editable_set_position(GTK_EDITABLE(self), -1);
    self->state->updating_text = false;
}

static void ladybird_location_entry_finalize(GObject* object)
{
    auto* self = LADYBIRD_LOCATION_ENTRY(object);
    if (self->popover) {
        gtk_popover_popdown(self->popover);
        gtk_widget_unparent(GTK_WIDGET(self->popover));
        self->popover = nullptr;
    }
    self->state.clear();
    G_OBJECT_CLASS(ladybird_location_entry_parent_class)->finalize(object);
}

static void ladybird_location_entry_class_init(LadybirdLocationEntryClass* klass)
{
    auto* object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = ladybird_location_entry_finalize;
}

static void ladybird_location_entry_init(LadybirdLocationEntry* self)
{
    self->state = adopt_own(*new LocationEntryState { .autocomplete = make<WebView::Autocomplete>(), .suggestions = {}, .user_text = {}, .on_navigate = {} });

    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);

    if (auto const& search_engine = WebView::Application::settings().search_engine(); search_engine.has_value()) {
        auto placeholder = ByteString::formatted("Search with {} or enter URL", search_engine->name);
        gtk_entry_set_placeholder_text(GTK_ENTRY(self), placeholder.characters());
    } else {
        gtk_entry_set_placeholder_text(GTK_ENTRY(self), "Enter URL or search...");
    }

    // Load completion popover from resource
    Ladybird::GObjectPtr builder { gtk_builder_new_from_resource("/org/ladybird/Ladybird/gtk/location-entry.ui") };
    self->popover = LadybirdWidgets::get_builder_object<GtkPopover>(builder, "completion_popover");
    self->list_box = LadybirdWidgets::get_builder_object<GtkListBox>(builder, "completion_list_box");
    gtk_widget_set_parent(GTK_WIDGET(self->popover), GTK_WIDGET(self));

    // Clicking a suggestion navigates to it
    g_signal_connect_swapped(self->list_box, "row-activated", G_CALLBACK(+[](LadybirdLocationEntry* self, GtkListBoxRow* row) {
        auto index = gtk_list_box_row_get_index(row);
        if (index >= 0 && static_cast<size_t>(index) < self->state->suggestions.size()) {
            set_entry_text_suppressed(self, self->state->suggestions[index].text.to_byte_string().characters());
            ladybird_location_entry_hide_completions(self);
            ladybird_location_entry_navigate(self);
        }
    }),
        self);

    // Autocomplete results callback
    self->state->autocomplete->on_autocomplete_query_complete = [self](auto suggestions, auto) {
        if (suggestions.is_empty() || !self->state->is_focused) {
            ladybird_location_entry_hide_completions(self);
            return;
        }
        self->state->suggestions = move(suggestions);
        self->state->selected_index = -1;
        ladybird_location_entry_show_completions(self);
    };

    // Text changed -> query autocomplete
    g_signal_connect_swapped(self, "changed", G_CALLBACK(+[](LadybirdLocationEntry* self, GtkEditable*) {
        if (!self->state->is_focused || self->state->updating_text)
            return;
        auto* text = gtk_editable_get_text(GTK_EDITABLE(self));
        if (!text || text[0] == '\0') {
            ladybird_location_entry_hide_completions(self);
            return;
        }
        self->state->user_text = MUST(String::from_utf8(StringView { text, strlen(text) }));
        self->state->autocomplete->query_autocomplete_engine(self->state->user_text);
    }),
        self);

    // Enter navigates
    g_signal_connect_swapped(self, "activate", G_CALLBACK(+[](LadybirdLocationEntry* self, GtkEntry*) {
        ladybird_location_entry_hide_completions(self);
        ladybird_location_entry_navigate(self);
    }),
        self);

    // Key controller for Up/Down/Escape
    auto* key_controller = gtk_event_controller_key_new();
    g_signal_connect_swapped(key_controller, "key-pressed", G_CALLBACK(+[](LadybirdLocationEntry* self, guint keyval, guint, GdkModifierType) -> gboolean {
        if (!gtk_widget_get_visible(GTK_WIDGET(self->popover)))
            return GDK_EVENT_PROPAGATE;

        switch (keyval) {
        case GDK_KEY_Down:
            ladybird_location_entry_move_selection(self, 1);
            return GDK_EVENT_STOP;
        case GDK_KEY_Up:
            ladybird_location_entry_move_selection(self, -1);
            return GDK_EVENT_STOP;
        case GDK_KEY_Escape:
            ladybird_location_entry_hide_completions(self);
            if (!self->state->user_text.is_empty())
                set_entry_text_suppressed(self, self->state->user_text.to_byte_string().characters());
            return GDK_EVENT_STOP;
        default:
            return GDK_EVENT_PROPAGATE;
        }
    }),
        self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(key_controller));

    // Focus tracking
    auto* focus_controller = gtk_event_controller_focus_new();
    g_signal_connect_swapped(focus_controller, "enter", G_CALLBACK(+[](LadybirdLocationEntry* self, GtkEventControllerFocus*) {
        self->state->is_focused = true;
        gtk_entry_set_attributes(GTK_ENTRY(self), nullptr);
    }),
        self);
    g_signal_connect_swapped(focus_controller, "leave", G_CALLBACK(+[](LadybirdLocationEntry* self, GtkEventControllerFocus*) {
        self->state->is_focused = false;
        ladybird_location_entry_hide_completions(self);
        ladybird_location_entry_update_display_attributes(self);
    }),
        self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(focus_controller));
}

// Public API

LadybirdLocationEntry* ladybird_location_entry_new(void)
{
    return LADYBIRD_LOCATION_ENTRY(g_object_new(LADYBIRD_TYPE_LOCATION_ENTRY, nullptr));
}

void ladybird_location_entry_set_url(LadybirdLocationEntry* self, char const* url)
{
    set_entry_text_suppressed(self, url ? url : "");

    // Extract scheme for security icon
    if (url) {
        auto sv = StringView(url, strlen(url));
        auto colon = sv.find(':');
        if (colon.has_value()) {
            auto scheme = sv.substring_view(0, *colon);
            auto scheme_bs = ByteString(scheme);
            ladybird_location_entry_set_security_icon(self, scheme_bs.characters());
        } else {
            ladybird_location_entry_set_security_icon(self, nullptr);
        }
    } else {
        ladybird_location_entry_set_security_icon(self, nullptr);
    }

    if (!self->state->is_focused)
        ladybird_location_entry_update_display_attributes(self);
}

void ladybird_location_entry_set_text(LadybirdLocationEntry* self, char const* text)
{
    set_entry_text_suppressed(self, text ? text : "");
    gtk_entry_set_attributes(GTK_ENTRY(self), nullptr);
    ladybird_location_entry_set_security_icon(self, nullptr);
}

void ladybird_location_entry_set_security_icon(LadybirdLocationEntry* self, char const* scheme)
{
    if (!scheme) {
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, nullptr);
        return;
    }

    auto sv = StringView(scheme, strlen(scheme));
    if (sv == "https"sv) {
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, "channel-secure-symbolic");
        gtk_entry_set_icon_tooltip_text(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, "Secure connection");
    } else if (sv == "file"sv || sv == "resource"sv || sv == "about"sv || sv == "data"sv) {
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, nullptr);
    } else {
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, "channel-insecure-symbolic");
        gtk_entry_set_icon_tooltip_text(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, "Insecure connection");
    }
}

void ladybird_location_entry_focus_and_select_all(LadybirdLocationEntry* self)
{
    gtk_widget_grab_focus(GTK_WIDGET(self));
    gtk_editable_select_region(GTK_EDITABLE(self), 0, -1);
}

void ladybird_location_entry_set_on_navigate(LadybirdLocationEntry* self, Function<void(String)> callback)
{
    self->state->on_navigate = move(callback);
}

// Internal helpers

static void ladybird_location_entry_update_display_attributes(LadybirdLocationEntry* self)
{
    auto* text = gtk_editable_get_text(GTK_EDITABLE(self));
    if (!text || text[0] == '\0') {
        gtk_entry_set_attributes(GTK_ENTRY(self), nullptr);
        return;
    }

    auto url_str = StringView(text, strlen(text));
    auto url_parts = WebView::break_url_into_parts(url_str);

    auto* attrs = pango_attr_list_new();

    if (url_parts.has_value()) {
        auto* dim = pango_attr_foreground_alpha_new(40000);
        pango_attr_list_insert(attrs, dim);

        auto highlight_start = url_parts->scheme_and_subdomain.length();
        auto highlight_end = highlight_start + url_parts->effective_tld_plus_one.length();

        if (highlight_start < highlight_end) {
            auto* domain_alpha = pango_attr_foreground_alpha_new(65535);
            domain_alpha->start_index = highlight_start;
            domain_alpha->end_index = highlight_end;
            pango_attr_list_insert(attrs, domain_alpha);

            auto* semi_bold = pango_attr_weight_new(PANGO_WEIGHT_MEDIUM);
            semi_bold->start_index = highlight_start;
            semi_bold->end_index = highlight_end;
            pango_attr_list_insert(attrs, semi_bold);
        }
    }

    gtk_entry_set_attributes(GTK_ENTRY(self), attrs);
    pango_attr_list_unref(attrs);
}

static void ladybird_location_entry_navigate(LadybirdLocationEntry* self)
{
    auto* text = gtk_editable_get_text(GTK_EDITABLE(self));
    if (!text || text[0] == '\0')
        return;
    auto query = MUST(String::from_utf8(StringView { text, strlen(text) }));
    if (auto url = WebView::sanitize_url(query, WebView::Application::settings().search_engine()); url.has_value()) {
        if (self->state->on_navigate)
            self->state->on_navigate(url->serialize());
    }
}

static void ladybird_location_entry_show_completions(LadybirdLocationEntry* self)
{
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box))) != nullptr)
        gtk_list_box_remove(self->list_box, child);

    for (auto const& suggestion : self->state->suggestions) {
        auto byte_str = suggestion.text.to_byte_string();
        auto* label = gtk_label_new(byte_str.characters());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_margin_start(label, 8);
        gtk_widget_set_margin_end(label, 8);
        gtk_widget_set_margin_top(label, 4);
        gtk_widget_set_margin_bottom(label, 4);
        gtk_list_box_append(self->list_box, label);
    }

    gtk_list_box_unselect_all(self->list_box);

    auto entry_width = gtk_widget_get_width(GTK_WIDGET(self));
    if (entry_width > 0)
        gtk_widget_set_size_request(GTK_WIDGET(self->popover), entry_width, -1);

    gtk_popover_popup(self->popover);
}

static void ladybird_location_entry_hide_completions(LadybirdLocationEntry* self)
{
    self->state->suggestions.clear();
    self->state->selected_index = -1;
    gtk_popover_popdown(self->popover);
}

static void ladybird_location_entry_move_selection(LadybirdLocationEntry* self, int delta)
{
    auto& state = *self->state;
    if (state.suggestions.is_empty())
        return;

    auto new_index = state.selected_index + delta;
    if (new_index < -1)
        new_index = static_cast<int>(state.suggestions.size()) - 1;
    if (new_index >= static_cast<int>(state.suggestions.size()))
        new_index = -1;

    state.selected_index = new_index;

    if (state.selected_index >= 0) {
        auto* row = gtk_list_box_get_row_at_index(self->list_box, state.selected_index);
        gtk_list_box_select_row(self->list_box, row);
        ladybird_location_entry_apply_selected_suggestion(self);
    } else {
        gtk_list_box_unselect_all(self->list_box);
        set_entry_text_suppressed(self, state.user_text.to_byte_string().characters(), true);
    }
}

static void ladybird_location_entry_apply_selected_suggestion(LadybirdLocationEntry* self)
{
    auto& state = *self->state;
    if (state.selected_index < 0 || static_cast<size_t>(state.selected_index) >= state.suggestions.size())
        return;

    set_entry_text_suppressed(self, state.suggestions[state.selected_index].text.to_byte_string().characters(), true);
}
