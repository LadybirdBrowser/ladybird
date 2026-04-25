/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/PrintSettings.h>
#include <UI/Gtk/Dialogs.h>
#include <UI/Gtk/GLibPtr.h>
#include <UI/Gtk/WebContentView.h>

#include <adwaita.h>

namespace Ladybird::Dialogs {

void show_error(GtkWindow* parent, StringView message)
{
    auto* dialog = adw_alert_dialog_new("Error", nullptr);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dialog), "%.*s",
        static_cast<int>(message.length()), message.characters_without_null_termination());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}

void show_alert(GtkWindow* parent, WebContentView* view, String const& message)
{
    auto* dialog = adw_alert_dialog_new("Alert", nullptr);
    auto msg = message.to_byte_string();
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog), msg.characters());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(+[](WebContentView* view, char const*) {
        view->alert_closed();
    }),
        view);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}

void show_confirm(GtkWindow* parent, WebContentView* view, String const& message)
{
    auto* dialog = adw_alert_dialog_new("Confirm", nullptr);
    auto msg = message.to_byte_string();
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog), msg.characters());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "ok", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "ok");
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(+[](WebContentView* view, char const* response) {
        view->confirm_closed(StringView(response, strlen(response)) == "ok"sv);
    }),
        view);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}

void show_prompt(GtkWindow* parent, WebContentView* view, String const& message, String const& default_value)
{
    auto* dialog = adw_alert_dialog_new("Prompt", nullptr);
    auto msg = message.to_byte_string();
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog), msg.characters());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "ok", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "ok");

    auto* entry = gtk_entry_new();
    auto def = default_value.to_byte_string();
    gtk_editable_set_text(GTK_EDITABLE(entry), def.characters());
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);

    struct PromptData {
        WebContentView* view;
        GtkEntry* entry;
    };
    auto* data = new PromptData { view, GTK_ENTRY(entry) };

    g_signal_connect(dialog, "response", G_CALLBACK(+[](AdwAlertDialog*, char const* response, gpointer user_data) {
        auto* data = static_cast<PromptData*>(user_data);
        if (StringView(response, strlen(response)) == "ok"sv) {
            auto* text = gtk_editable_get_text(GTK_EDITABLE(data->entry));
            data->view->prompt_closed(MUST(String::from_utf8(StringView(text, strlen(text)))));
        } else {
            data->view->prompt_closed({});
        }
        delete data;
    }),
        data);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}

void show_color_picker(GtkWindow* parent, WebContentView* view, Color current_color)
{
    GObjectPtr dialog { gtk_color_dialog_new() };
    auto rgba = GdkRGBA {
        static_cast<float>(current_color.red()) / 255.0f,
        static_cast<float>(current_color.green()) / 255.0f,
        static_cast<float>(current_color.blue()) / 255.0f,
        static_cast<float>(current_color.alpha()) / 255.0f
    };

    gtk_color_dialog_choose_rgba(GTK_COLOR_DIALOG(dialog.ptr()), parent, &rgba, nullptr, +[](GObject* source, GAsyncResult* result, gpointer user_data) {
            auto* view = static_cast<WebContentView*>(user_data);
            GError* error = nullptr;
            auto* color = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(source), result, &error);
            if (error) {
                view->color_picker_update({}, Web::HTML::ColorPickerUpdateState::Closed);
                g_error_free(error);
                return;
            }
            auto picked = Color(
                static_cast<u8>(color->red * 255),
                static_cast<u8>(color->green * 255),
                static_cast<u8>(color->blue * 255),
                static_cast<u8>(color->alpha * 255));
            view->color_picker_update(picked, Web::HTML::ColorPickerUpdateState::Closed);
            gdk_rgba_free(color); }, view);
}

void show_file_picker(GtkWindow* parent, WebContentView* view, Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple)
{
    GObjectPtr dialog { gtk_file_dialog_new() };
    if (allow_multiple == Web::HTML::AllowMultipleFiles::Yes)
        gtk_file_dialog_set_title(GTK_FILE_DIALOG(dialog.ptr()), "Select Files");
    else
        gtk_file_dialog_set_title(GTK_FILE_DIALOG(dialog.ptr()), "Select File");

    // Build file filters from accepted types
    GObjectPtr filters { g_list_store_new(GTK_TYPE_FILE_FILTER) };
    if (!accepted_file_types.filters.is_empty()) {
        GObjectPtr filter { gtk_file_filter_new() };
        gtk_file_filter_set_name(GTK_FILE_FILTER(filter.ptr()), "Accepted files");
        for (auto const& filter_type : accepted_file_types.filters) {
            filter_type.visit(
                [&](Web::HTML::FileFilter::Extension const& ext) {
                    auto pattern = ByteString::formatted("*.{}", ext.value);
                    gtk_file_filter_add_pattern(GTK_FILE_FILTER(filter.ptr()), pattern.characters());
                },
                [&](Web::HTML::FileFilter::MimeType const& mime) {
                    auto mime_str = mime.value.to_byte_string();
                    gtk_file_filter_add_mime_type(GTK_FILE_FILTER(filter.ptr()), mime_str.characters());
                },
                [&](Web::HTML::FileFilter::FileType const& file_type) {
                    switch (file_type) {
                    case Web::HTML::FileFilter::FileType::Audio:
                        gtk_file_filter_add_mime_type(GTK_FILE_FILTER(filter.ptr()), "audio/*");
                        break;
                    case Web::HTML::FileFilter::FileType::Image:
                        gtk_file_filter_add_mime_type(GTK_FILE_FILTER(filter.ptr()), "image/*");
                        break;
                    case Web::HTML::FileFilter::FileType::Video:
                        gtk_file_filter_add_mime_type(GTK_FILE_FILTER(filter.ptr()), "video/*");
                        break;
                    }
                });
        }
        g_list_store_append(G_LIST_STORE(filters.ptr()), filter.ptr());
    }
    GObjectPtr all_filter { gtk_file_filter_new() };
    gtk_file_filter_set_name(GTK_FILE_FILTER(all_filter.ptr()), "All files");
    gtk_file_filter_add_pattern(GTK_FILE_FILTER(all_filter.ptr()), "*");
    g_list_store_append(G_LIST_STORE(filters.ptr()), all_filter.ptr());
    gtk_file_dialog_set_filters(GTK_FILE_DIALOG(dialog.ptr()), G_LIST_MODEL(filters.ptr()));

    if (allow_multiple == Web::HTML::AllowMultipleFiles::Yes) {
        gtk_file_dialog_open_multiple(GTK_FILE_DIALOG(dialog.ptr()), parent, nullptr, +[](GObject* source, GAsyncResult* result, gpointer user_data) {
                auto* view = static_cast<WebContentView*>(user_data);
                GError* error = nullptr;
                auto* file_list = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, &error);
                if (error) {
                    view->file_picker_closed({});
                    g_error_free(error);
                    return;
                }
                GObjectPtr owned_file_list { file_list };
                Vector<Web::HTML::SelectedFile> selected;
                auto n = g_list_model_get_n_items(G_LIST_MODEL(file_list));
                for (guint i = 0; i < n; i++) {
                    GObjectPtr file { g_list_model_get_item(G_LIST_MODEL(file_list), i) };
                    g_autofree char* path = g_file_get_path(G_FILE(file.ptr()));
                    if (path) {
                        auto selected_file = Web::HTML::SelectedFile::from_file_path(ByteString(path));
                        if (!selected_file.is_error())
                            selected.append(selected_file.release_value());
                    }
                }
                view->file_picker_closed(move(selected)); }, view);
    } else {
        gtk_file_dialog_open(GTK_FILE_DIALOG(dialog.ptr()), parent, nullptr, +[](GObject* source, GAsyncResult* result, gpointer user_data) {
                auto* view = static_cast<WebContentView*>(user_data);
                GError* error = nullptr;
                auto* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
                if (error) {
                    view->file_picker_closed({});
                    g_error_free(error);
                    return;
                }
                GObjectPtr owned_file { file };
                Vector<Web::HTML::SelectedFile> selected;
                g_autofree char* path = g_file_get_path(file);
                if (path) {
                    auto selected_file = Web::HTML::SelectedFile::from_file_path(ByteString(path));
                    if (!selected_file.is_error())
                        selected.append(selected_file.release_value());
                }
                view->file_picker_closed(move(selected)); }, view);
    }
}

struct PrintData {
    WebContentView* view { nullptr };
    Gfx::ShareableBitmap bitmap;
    int pages { 0 };
    double scale { 1.0 };
};

void show_print_dialog(GtkWindow* parent, WebContentView* view)
{
    GObjectPtr<GtkPrintOperation> op { gtk_print_operation_new() };
    gtk_print_operation_set_job_name(op.ptr(), "Ladybird");
    gtk_print_operation_set_track_print_status(op.ptr(), TRUE);

    auto* data = new PrintData;
    data->view = view;

    // Temporarily claim on_request_print_bitmap to deliver bitmap into PrintData.
    view->on_request_print_bitmap = [data](Gfx::ShareableBitmap bmp) {
        data->bitmap = bmp;
    };

    g_signal_connect(op.ptr(), "begin-print", G_CALLBACK(+[](GtkPrintOperation* op, GtkPrintContext* context, gpointer user_data) {
        auto* data = static_cast<PrintData*>(user_data);
        // GtkPrintContext dimensions are in points (1 pt = 0.352778 mm)
        constexpr float pts_to_mm = 0.352778f;
        Web::HTML::PrintSettings settings;
        settings.paper_width_mm = static_cast<float>(gtk_print_context_get_width(context)) * pts_to_mm;
        settings.paper_height_mm = static_cast<float>(gtk_print_context_get_height(context)) * pts_to_mm;
        // GtkPrintContext dimensions already exclude margins; use zero margins so
        // with_print_mode renders exactly to the available area.
        settings.margin_top_mm = 0.0f;
        settings.margin_right_mm = 0.0f;
        settings.margin_bottom_mm = 0.0f;
        settings.margin_left_mm = 0.0f;

        data->view->trigger_print(settings);

        // Spin the GLib main loop until WebContent delivers the bitmap.
        while (!data->bitmap.is_valid())
            g_main_context_iteration(nullptr, TRUE);

        double page_width = gtk_print_context_get_width(context);
        double page_height = gtk_print_context_get_height(context);
        double bmp_width = data->bitmap.bitmap()->width();
        data->scale = (bmp_width > 0) ? page_width / bmp_width : 1.0;
        double scaled_height = data->bitmap.bitmap()->height() * data->scale;
        data->pages = static_cast<int>(ceil(scaled_height / page_height));
        if (data->pages < 1)
            data->pages = 1;
        gtk_print_operation_set_n_pages(op, data->pages);
    }),
        data);

    g_signal_connect(op.ptr(), "draw-page", G_CALLBACK(+[](GtkPrintOperation*, GtkPrintContext* context, gint page_num, gpointer user_data) {
        auto* data = static_cast<PrintData*>(user_data);
        auto* bmp = data->bitmap.bitmap();
        if (!bmp)
            return;
        auto* cr = gtk_print_context_get_cairo_context(context);
        double page_height = gtk_print_context_get_height(context);

        // Paint bitmap via a Cairo image surface (BGRA8888 matches CAIRO_FORMAT_ARGB32 on LE)
        int width = bmp->width();
        int height = bmp->height();
        auto* surface = cairo_image_surface_create_for_data(
            const_cast<unsigned char*>(bmp->scanline_u8(0)),
            CAIRO_FORMAT_ARGB32,
            width, height, bmp->pitch());
        if (!surface)
            return;

        cairo_save(cr);
        cairo_scale(cr, data->scale, data->scale);
        double y_offset = -page_num * (page_height / data->scale);
        cairo_set_source_surface(cr, surface, 0, y_offset);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(surface);
    }),
        data);

    g_signal_connect_swapped(op.ptr(), "done", G_CALLBACK(+[](PrintData* data) {
        data->view->on_request_print_bitmap = {};
        if (data->view)
            data->view->did_finish_print();
        delete data;
    }),
        data);

    g_autoptr(GError) error = nullptr;
    gtk_print_operation_run(op.ptr(), GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, parent, &error);
}

}
