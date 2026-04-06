/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    register_interface("showContextMenu"sv, [this](auto const& data) {
        show_context_menu(data);
    });
}

void BookmarksUI::bookmarks_changed()
{
    load_bookmarks();
}

void BookmarksUI::load_bookmarks()
{
    async_send_message("loadBookmarks"sv, Application::bookmark_store().serialize_items());
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

        Application::the().show_bookmark_context_menu({ *client_x, *client_y }, item, target_folder_id);
    } else {
        Application::the().show_bookmark_context_menu({ *client_x, *client_y }, {}, {});
    }
}

}
