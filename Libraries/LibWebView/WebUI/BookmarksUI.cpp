/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibCore/Promise.h>
#include <LibWebView/Application.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/WebUI/BookmarksUI.h>

namespace WebView {

void BookmarksUI::register_interfaces()
{
    register_interface("loadBookmarks"sv, [this](auto const&) {
        load_bookmarks();
    });
    register_interface("moveItem"sv, [this](auto const& data) {
        move_item(data);
    });
    register_interface("newFolder"sv, [this](auto const& data) {
        new_folder(data);
    });
    register_interface("showContextMenu"sv, [this](auto const& data) {
        show_context_menu(data);
    });
    register_interface("importBookmarks"sv, [this](auto const& data) {
        import_bookmarks(data);
    });
    register_interface("exportBookmarks"sv, [this](auto const& data) {
        export_bookmarks(data);
    });
}

void BookmarksUI::bookmarks_changed()
{
    load_bookmarks();
}

void BookmarksUI::load_bookmarks()
{
    async_send_message("loadBookmarks"sv, Application::bookmark_store().serialize_items(*Application::default_session().favicon_store));
}

void BookmarksUI::move_item(JsonValue const& data)
{
    if (!data.is_object())
        return;
    auto const& object = data.as_object();

    auto id = object.get_string("id"sv);
    auto index = object.get_integer<size_t>("index"sv);
    if (!id.has_value() || !index.has_value())
        return;

    auto target_folder_id = object.get_string("targetFolderId"sv);
    Application::bookmark_store().move_item(*id, target_folder_id, *index);
}

void BookmarksUI::new_folder(JsonValue const& data)
{
    Optional<String> target_folder_id;
    if (data.is_object()) {
        if (auto folder_id = data.as_object().get_string("targetFolderId"sv); folder_id.has_value())
            target_folder_id = *folder_id;
    }

    Application::the().display_add_bookmark_folder_dialog({}, target_folder_id)->when_resolved([](Application::BookmarkDialogResult result) {
        auto& folder = result.data.get<BookmarkItem::Folder>();
        Application::bookmark_store().add_folder(move(folder.title), result.target_folder_id);
    });
}

void BookmarksUI::import_bookmarks(JsonValue const& data)
{
    if (!data.is_array())
        return;

    Application::bookmark_store().import_items(data.as_array());
}

void BookmarksUI::export_bookmarks(JsonValue const& data)
{
    if (!data.is_string())
        return;

    auto destination = Application::the().path_for_downloaded_file(ByteString { "bookmarks.html" });
    if (destination.is_error())
        return;

    auto file = Core::File::open(destination.value().string(), Core::File::OpenMode::Write);
    if (file.is_error()) {
        Application::the().display_error_dialog(MUST(String::formatted("Unable to export bookmarks: {}", file.error())));
        return;
    }

    if (auto result = file.value()->write_until_depleted(data.as_string().bytes()); result.is_error()) {
        Application::the().display_error_dialog(MUST(String::formatted("Unable to export bookmarks: {}", result.error())));
        return;
    }
}

void BookmarksUI::show_context_menu(JsonValue const& data)
{
    if (!data.is_object())
        return;
    auto const& object = data.as_object();

    auto client_x = object.get_integer<i32>("clientX"sv);
    auto client_y = object.get_integer<i32>("clientY"sv);
    if (!client_x.has_value() || !client_y.has_value())
        return;

    if (auto id = object.get_string("id"sv); id.has_value()) {
        auto item = Application::bookmark_store().find_item_by_id(*id);
        auto target_folder_id = object.get_string("targetFolderId"sv);
        auto parent_folder_id = object.get_string("parentFolderId"sv);

        Application::the().show_bookmark_context_menu({ *client_x, *client_y }, item, target_folder_id, parent_folder_id);
    } else {
        // NB: Empty space shows the menu for the folder being shown.
        auto target_folder_id = object.get_string("targetFolderId"sv);
        Application::the().show_bookmark_context_menu({ *client_x, *client_y }, {}, target_folder_id, {});
    }
}

}
