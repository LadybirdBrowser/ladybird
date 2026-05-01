/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
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

struct RowInfo {
    bool is_section_header;
    size_t suggestion_index; // only meaningful when !is_section_header
};

struct LocationEntryState {
    NonnullOwnPtr<WebView::Autocomplete> autocomplete;
    Vector<WebView::AutocompleteSuggestion> suggestions;
    Ladybird::GObjectPtr<GdkPaintable> favicon;
    Vector<RowInfo> row_info;
    int selected_row { -1 }; // row index in list_box (-1 = none)
    String user_text;
    bool is_focused { false };
    bool is_loading { false };
    bool updating_text { false };
    guint loading_pulse_source_id { 0 };
    // Inline autocomplete state
    String current_inline_suggestion;
    Optional<String> suppressed_query;
    bool suppress_on_next_change { false };
    bool applying_inline { false };
    Function<void(String)> on_navigate;
};

static Optional<String> inline_completion_for_suggestion(StringView query, StringView suggestion_url)
{
    // Strip trailing root slash: "example.com/" → "example.com" (but not "example.com/path/")
    StringView candidate = suggestion_url;
    if (candidate.ends_with('/')) {
        auto without_slash = candidate.substring_view(0, candidate.length() - 1);
        if (!without_slash.contains('/'))
            candidate = without_slash;
    }

    auto try_prefix = [&](StringView url) -> Optional<String> {
        if (!WebView::autocomplete_url_can_complete(query, url))
            return {};
        return MUST(String::formatted("{}{}", query, url.substring_view(query.length())));
    };

    if (auto r = try_prefix(candidate); r.has_value())
        return r;
    if (candidate.starts_with("www."sv))
        if (auto r = try_prefix(candidate.substring_view(4)); r.has_value())
            return r;
    for (auto scheme : { "https://"sv, "http://"sv }) {
        if (!candidate.starts_with(scheme))
            continue;
        auto rest = candidate.substring_view(scheme.length());
        if (auto r = try_prefix(rest); r.has_value())
            return r;
        if (rest.starts_with("www."sv))
            if (auto r = try_prefix(rest.substring_view(4)); r.has_value())
                return r;
    }
    return {};
}

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
static int ladybird_location_entry_apply_inline_autocomplete(LadybirdLocationEntry* self, Vector<WebView::AutocompleteSuggestion> const& suggestions);
static void ladybird_location_entry_apply_inline_text(LadybirdLocationEntry* self, StringView inline_text, StringView query);
static void ladybird_location_entry_restore_query(LadybirdLocationEntry* self);
static void ladybird_location_entry_reset_inline_state(LadybirdLocationEntry* self);
static String ladybird_location_entry_current_query(LadybirdLocationEntry* self);
static void ladybird_location_entry_apply_selected_suggestion(LadybirdLocationEntry* self);
static void ladybird_location_entry_update_leading_icon(LadybirdLocationEntry* self);

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
    if (self->state && self->state->loading_pulse_source_id != 0) {
        g_source_remove(self->state->loading_pulse_source_id);
        self->state->loading_pulse_source_id = 0;
    }
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
    self->state = adopt_own(*new LocationEntryState {
        .autocomplete = make<WebView::Autocomplete>(),
        .suggestions = {},
        .favicon = {},
        .row_info = {},
        .selected_row = -1,
        .user_text = {},
        .is_focused = false,
        .is_loading = false,
        .updating_text = false,
        .loading_pulse_source_id = 0,
        .current_inline_suggestion = {},
        .suppressed_query = {},
        .suppress_on_next_change = false,
        .applying_inline = false,
        .on_navigate = {},
    });

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
        auto row_index = gtk_list_box_row_get_index(row);
        if (row_index < 0 || static_cast<size_t>(row_index) >= self->state->row_info.size())
            return;
        auto const& info = self->state->row_info[row_index];
        if (info.is_section_header)
            return;
        set_entry_text_suppressed(self, self->state->suggestions[info.suggestion_index].text.to_byte_string().characters());
        ladybird_location_entry_hide_completions(self);
        ladybird_location_entry_navigate(self);
    }),
        self);

    // Autocomplete results callback
    self->state->autocomplete->on_autocomplete_query_complete = [self](auto suggestions, auto result_kind) {
        if (suggestions.is_empty() || !self->state->is_focused) {
            ladybird_location_entry_hide_completions(self);
            return;
        }

        int selected_row = ladybird_location_entry_apply_inline_autocomplete(self, suggestions);

        bool popup_visible = gtk_widget_get_visible(GTK_WIDGET(self->popover));
        if (result_kind == WebView::AutocompleteResultKind::Intermediate && popup_visible) {
            if (self->state->selected_row >= 0
                && static_cast<size_t>(self->state->selected_row) < self->state->row_info.size()) {
                auto const& info = self->state->row_info[self->state->selected_row];
                if (!info.is_section_header) {
                    auto const& current_text = self->state->suggestions[info.suggestion_index].text;
                    for (auto const& suggestion : suggestions) {
                        if (suggestion.text == current_text)
                            return;
                    }
                }
            }
            self->state->selected_row = -1;
            gtk_list_box_unselect_all(self->list_box);
            return;
        }

        self->state->suggestions = move(suggestions);
        self->state->selected_row = -1;
        ladybird_location_entry_show_completions(self);

        if (selected_row >= 0) {
            for (size_t i = 0; i < self->state->row_info.size(); ++i) {
                auto const& info = self->state->row_info[i];
                if (!info.is_section_header && info.suggestion_index == static_cast<size_t>(selected_row)) {
                    self->state->selected_row = static_cast<int>(i);
                    auto* row = gtk_list_box_get_row_at_index(self->list_box, static_cast<int>(i));
                    if (row)
                        gtk_list_box_select_row(self->list_box, row);
                    break;
                }
            }
        }
    };

    // Text changed -> query autocomplete
    g_signal_connect_swapped(self, "changed", G_CALLBACK(+[](LadybirdLocationEntry* self, GtkEditable*) {
        if (!self->state->is_focused || self->state->updating_text || self->state->applying_inline)
            return;
        auto* text = gtk_editable_get_text(GTK_EDITABLE(self));
        if (!text || text[0] == '\0') {
            ladybird_location_entry_hide_completions(self);
            return;
        }

        auto query = ladybird_location_entry_current_query(self);

        if (self->state->suppress_on_next_change) {
            self->state->suppressed_query = query;
            self->state->suppress_on_next_change = false;
        } else if (self->state->suppressed_query.has_value() && self->state->suppressed_query.value() != query) {
            self->state->suppressed_query = {};
        }

        if (!self->state->suppressed_query.has_value() && !self->state->current_inline_suggestion.is_empty()) {
            auto query_sv = query.bytes_as_string_view();
            auto completion = inline_completion_for_suggestion(query_sv, self->state->current_inline_suggestion.bytes_as_string_view());
            if (!completion.has_value() || completion.value() == query) {
                ladybird_location_entry_restore_query(self);
                self->state->current_inline_suggestion = {};
            } else {
                ladybird_location_entry_apply_inline_text(self, completion.value(), query_sv);
            }
        }

        self->state->user_text = query;
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
        if (keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Delete)
            self->state->suppress_on_next_change = true;

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
            ladybird_location_entry_reset_inline_state(self);
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
        self->state->autocomplete->cancel_pending_query();
        ladybird_location_entry_hide_completions(self);
        ladybird_location_entry_reset_inline_state(self);
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

    if (!self->state->is_focused)
        ladybird_location_entry_update_display_attributes(self);
}

void ladybird_location_entry_set_text(LadybirdLocationEntry* self, char const* text)
{
    set_entry_text_suppressed(self, text ? text : "");
    gtk_entry_set_attributes(GTK_ENTRY(self), nullptr);
    ladybird_location_entry_set_favicon(self, nullptr);
}

void ladybird_location_entry_set_favicon(LadybirdLocationEntry* self, GdkPaintable* favicon)
{
    self->state->favicon = Ladybird::GObjectPtr<GdkPaintable>(favicon ? GDK_PAINTABLE(g_object_ref(favicon)) : nullptr);
    ladybird_location_entry_update_leading_icon(self);
}

void ladybird_location_entry_set_loading(LadybirdLocationEntry* self, bool is_loading)
{
    if (self->state->is_loading == is_loading)
        return;

    self->state->is_loading = is_loading;
    if (self->state->is_loading) {
        gtk_entry_set_progress_pulse_step(GTK_ENTRY(self), 0.15);
        self->state->loading_pulse_source_id = g_timeout_add_full(
            G_PRIORITY_DEFAULT,
            100,
            +[](gpointer user_data) -> gboolean {
                auto* self = LADYBIRD_LOCATION_ENTRY(user_data);
                gtk_entry_progress_pulse(GTK_ENTRY(self));
                return G_SOURCE_CONTINUE;
            },
            self,
            nullptr);
    } else {
        if (self->state->loading_pulse_source_id != 0) {
            g_source_remove(self->state->loading_pulse_source_id);
            self->state->loading_pulse_source_id = 0;
        }
        gtk_entry_set_progress_fraction(GTK_ENTRY(self), 0.0);
    }

    ladybird_location_entry_update_leading_icon(self);
}

static void ladybird_location_entry_update_leading_icon(LadybirdLocationEntry* self)
{
    if (self->state->is_loading) {
        gtk_entry_set_icon_from_icon_name(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, "process-working-symbolic");
        gtk_entry_set_icon_tooltip_text(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, "Loading");
        return;
    }

    if (self->state->favicon.ptr()) {
        gtk_entry_set_icon_from_paintable(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, self->state->favicon.ptr());
        gtk_entry_set_icon_tooltip_text(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, "Page icon");
        return;
    }

    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, nullptr);
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(self), GTK_ENTRY_ICON_PRIMARY, nullptr);
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

static constexpr int AUTOCOMPLETE_ICON_SIZE = 16;

static GtkWidget* make_suggestion_icon(WebView::AutocompleteSuggestion const& suggestion)
{
    if (suggestion.source == WebView::AutocompleteSuggestionSource::Search) {
        auto* image = gtk_image_new_from_icon_name("edit-find-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(image), AUTOCOMPLETE_ICON_SIZE);
        return image;
    }

    if (suggestion.favicon_base64_png.has_value()) {
        auto decoded = decode_base64(*suggestion.favicon_base64_png);
        if (!decoded.is_error()) {
            auto& bytes = decoded.value();
            g_autoptr(GBytes) gbytes = g_bytes_new(bytes.data(), bytes.size());
            g_autoptr(GError) error = nullptr;
            Ladybird::GObjectPtr<GdkTexture> texture { gdk_texture_new_from_bytes(gbytes, &error) };
            if (texture.ptr()) {
                auto* image = gtk_image_new_from_paintable(GDK_PAINTABLE(texture.ptr()));
                gtk_image_set_pixel_size(GTK_IMAGE(image), AUTOCOMPLETE_ICON_SIZE);
                return image;
            }
        }
    }

    auto* image = gtk_image_new_from_icon_name("web-browser-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(image), AUTOCOMPLETE_ICON_SIZE);
    return image;
}

static GtkWidget* make_suggestion_row_widget(WebView::AutocompleteSuggestion const& suggestion)
{
    auto* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(row_box, 8);
    gtk_widget_set_margin_end(row_box, 8);
    gtk_widget_set_margin_top(row_box, 5);
    gtk_widget_set_margin_bottom(row_box, 5);

    gtk_box_append(GTK_BOX(row_box), make_suggestion_icon(suggestion));

    if (suggestion.title.has_value()) {
        auto* text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(text_box, TRUE);

        auto title_str = suggestion.title->to_byte_string();
        auto* title_label = gtk_label_new(title_str.characters());
        gtk_label_set_xalign(GTK_LABEL(title_label), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(title_label, "caption-heading");
        gtk_box_append(GTK_BOX(text_box), title_label);

        auto secondary_str = (suggestion.subtitle.has_value() ? *suggestion.subtitle : suggestion.text).to_byte_string();
        auto* url_label = gtk_label_new(secondary_str.characters());
        gtk_label_set_xalign(GTK_LABEL(url_label), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(url_label), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(url_label, "dim-label");
        gtk_widget_add_css_class(url_label, "caption");
        gtk_box_append(GTK_BOX(text_box), url_label);

        gtk_box_append(GTK_BOX(row_box), text_box);
    } else {
        auto url_str = suggestion.text.to_byte_string();
        auto* url_label = gtk_label_new(url_str.characters());
        gtk_label_set_xalign(GTK_LABEL(url_label), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(url_label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(url_label, TRUE);
        gtk_box_append(GTK_BOX(row_box), url_label);
    }

    return row_box;
}

static void ladybird_location_entry_show_completions(LadybirdLocationEntry* self)
{
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_box))) != nullptr)
        gtk_list_box_remove(self->list_box, child);

    auto& state = *self->state;
    state.row_info.clear();

    auto current_section = WebView::AutocompleteSuggestionSection::None;
    for (size_t i = 0; i < state.suggestions.size(); ++i) {
        auto const& suggestion = state.suggestions[i];

        if (suggestion.section != WebView::AutocompleteSuggestionSection::None
            && suggestion.section != current_section) {
            current_section = suggestion.section;

            auto section_title = WebView::autocomplete_section_title(current_section);
            auto title_str = ByteString(section_title);
            auto* header_label = gtk_label_new(title_str.characters());
            gtk_label_set_xalign(GTK_LABEL(header_label), 0.0);
            gtk_widget_set_margin_start(header_label, 10);
            gtk_widget_set_margin_end(header_label, 10);
            gtk_widget_set_margin_top(header_label, 4);
            gtk_widget_set_margin_bottom(header_label, 4);
            gtk_widget_add_css_class(header_label, "dim-label");
            gtk_widget_add_css_class(header_label, "caption");

            auto* header_row = gtk_list_box_row_new();
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(header_row), FALSE);
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(header_row), FALSE);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(header_row), header_label);
            gtk_list_box_append(self->list_box, header_row);
            state.row_info.append({ .is_section_header = true, .suggestion_index = 0 });
        }

        auto* row_widget = make_suggestion_row_widget(suggestion);
        auto* row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_widget);
        gtk_list_box_append(self->list_box, row);
        state.row_info.append({ .is_section_header = false, .suggestion_index = i });
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
    self->state->row_info.clear();
    self->state->selected_row = -1;
    gtk_popover_popdown(self->popover);
}

static int ladybird_location_entry_step_to_selectable_row(LadybirdLocationEntry* self, int from, int direction)
{
    auto& state = *self->state;
    int n = static_cast<int>(state.row_info.size());
    if (n == 0)
        return -1;
    int candidate = from;
    for (int attempt = 0; attempt < n; ++attempt) {
        candidate += direction;
        if (candidate < 0)
            candidate = n - 1;
        else if (candidate >= n)
            candidate = 0;
        if (!state.row_info[candidate].is_section_header)
            return candidate;
    }
    return -1;
}

static void ladybird_location_entry_move_selection(LadybirdLocationEntry* self, int delta)
{
    auto& state = *self->state;
    if (state.row_info.is_empty())
        return;

    int new_row = ladybird_location_entry_step_to_selectable_row(self, state.selected_row, delta);

    state.selected_row = new_row;

    if (new_row >= 0) {
        auto* row = gtk_list_box_get_row_at_index(self->list_box, new_row);
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
    if (state.selected_row < 0 || static_cast<size_t>(state.selected_row) >= state.row_info.size())
        return;
    auto const& info = state.row_info[state.selected_row];
    if (info.is_section_header)
        return;
    set_entry_text_suppressed(self, state.suggestions[info.suggestion_index].text.to_byte_string().characters(), true);
}

static String ladybird_location_entry_current_query(LadybirdLocationEntry* self)
{
    auto* text = gtk_editable_get_text(GTK_EDITABLE(self));
    if (!text || text[0] == '\0')
        return {};
    size_t text_bytes = strlen(text);
    int text_chars = static_cast<int>(g_utf8_strlen(text, -1));
    int start, end;
    if (!gtk_editable_get_selection_bounds(GTK_EDITABLE(self), &start, &end))
        return MUST(String::from_utf8(StringView(text, text_bytes)));
    if (end != text_chars)
        return MUST(String::from_utf8(StringView(text, text_bytes)));
    size_t start_bytes = static_cast<size_t>(g_utf8_offset_to_pointer(text, start) - text);
    return MUST(String::from_utf8(StringView(text, start_bytes)));
}

static void ladybird_location_entry_apply_inline_text(LadybirdLocationEntry* self, StringView inline_text, StringView query)
{
    if (!self->state->is_focused)
        return;
    int completion_start = static_cast<int>(g_utf8_strlen(query.characters_without_null_termination(), static_cast<gssize>(query.length())));
    int completion_end = static_cast<int>(g_utf8_strlen(inline_text.characters_without_null_termination(), static_cast<gssize>(inline_text.length())));
    if (completion_end <= completion_start)
        return;
    auto* current_text = gtk_editable_get_text(GTK_EDITABLE(self));
    int cur_start, cur_end;
    bool has_selection = gtk_editable_get_selection_bounds(GTK_EDITABLE(self), &cur_start, &cur_end);
    auto inline_bs = ByteString(inline_text);
    if (current_text && inline_bs == current_text && has_selection
        && cur_start == completion_start && cur_end == completion_end)
        return;
    self->state->applying_inline = true;
    gtk_editable_set_text(GTK_EDITABLE(self), inline_bs.characters());
    gtk_editable_select_region(GTK_EDITABLE(self), completion_start, completion_end);
    self->state->applying_inline = false;
}

static void ladybird_location_entry_restore_query(LadybirdLocationEntry* self)
{
    if (!self->state->is_focused)
        return;
    auto query = ladybird_location_entry_current_query(self);
    auto* current_text = gtk_editable_get_text(GTK_EDITABLE(self));
    int start, end;
    bool has_selection = gtk_editable_get_selection_bounds(GTK_EDITABLE(self), &start, &end);
    if (current_text && query.bytes_as_string_view() == StringView(current_text, strlen(current_text)) && !has_selection)
        return;
    auto query_bs = query.to_byte_string();
    self->state->applying_inline = true;
    gtk_editable_set_text(GTK_EDITABLE(self), query_bs.characters());
    gtk_editable_set_position(GTK_EDITABLE(self), static_cast<int>(g_utf8_strlen(query_bs.characters(), -1)));
    self->state->applying_inline = false;
}

static void ladybird_location_entry_reset_inline_state(LadybirdLocationEntry* self)
{
    self->state->current_inline_suggestion = {};
    self->state->suppressed_query = {};
    self->state->suppress_on_next_change = false;
}

static int ladybird_location_entry_apply_inline_autocomplete(LadybirdLocationEntry* self, Vector<WebView::AutocompleteSuggestion> const& suggestions)
{
    if (self->state->applying_inline || !self->state->is_focused)
        return -1;

    auto* text = gtk_editable_get_text(GTK_EDITABLE(self));
    size_t text_bytes = text ? strlen(text) : 0;
    int text_chars = text ? static_cast<int>(g_utf8_strlen(text, -1)) : 0;

    String query;
    int start, end;
    if (!gtk_editable_get_selection_bounds(GTK_EDITABLE(self), &start, &end)) {
        if (gtk_editable_get_position(GTK_EDITABLE(self)) != text_chars)
            return -1;
        query = MUST(String::from_utf8(StringView(text, text_bytes)));
    } else {
        if (end != text_chars)
            return -1;
        size_t start_bytes = static_cast<size_t>(g_utf8_offset_to_pointer(text, start) - text);
        query = MUST(String::from_utf8(StringView(text, start_bytes)));
    }

    if (suggestions.is_empty())
        return -1;

    auto& state = *self->state;
    auto query_sv = query.bytes_as_string_view();

    if (suggestions.first().source == WebView::AutocompleteSuggestionSource::LiteralURL) {
        state.current_inline_suggestion = {};
        int cur_start2, cur_end2;
        if (gtk_editable_get_selection_bounds(GTK_EDITABLE(self), &cur_start2, &cur_end2)
            || (text && query_sv != StringView(text, text_bytes)))
            ladybird_location_entry_restore_query(self);
        return 0;
    }

    if (state.suppressed_query.has_value() && state.suppressed_query.value() == query) {
        state.current_inline_suggestion = {};
        int cur_start2, cur_end2;
        if (gtk_editable_get_selection_bounds(GTK_EDITABLE(self), &cur_start2, &cur_end2)
            || (text && query_sv != StringView(text, text_bytes)))
            ladybird_location_entry_restore_query(self);
        return 0;
    }

    if (!state.current_inline_suggestion.is_empty()) {
        int preserved = -1;
        for (size_t i = 0; i < suggestions.size(); ++i) {
            if (suggestions[i].text == state.current_inline_suggestion) {
                preserved = static_cast<int>(i);
                break;
            }
        }
        if (preserved != -1) {
            auto preserved_inline = inline_completion_for_suggestion(query_sv, state.current_inline_suggestion.bytes_as_string_view());
            if (preserved_inline.has_value()) {
                ladybird_location_entry_apply_inline_text(self, preserved_inline.value(), query_sv);
                return preserved;
            }
        }
    }

    auto const& first_text = suggestions.first().text;
    auto first_inline = inline_completion_for_suggestion(query_sv, first_text.bytes_as_string_view());
    if (first_inline.has_value()) {
        state.current_inline_suggestion = first_text;
        ladybird_location_entry_apply_inline_text(self, first_inline.value(), query_sv);
        return 0;
    }

    state.current_inline_suggestion = {};
    int cur_start2, cur_end2;
    if (gtk_editable_get_selection_bounds(GTK_EDITABLE(self), &cur_start2, &cur_end2)
        || (text && query_sv != StringView(text, text_bytes)))
        ladybird_location_entry_restore_query(self);
    return 0;
}
