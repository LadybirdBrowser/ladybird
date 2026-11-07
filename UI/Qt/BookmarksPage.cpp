/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibCore/File.h>
#include <LibCore/Promise.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BookmarkStore.h>
#include <UI/Qt/Bookmarks.h>
#include <UI/Qt/BookmarksPage.h>
#include <UI/Qt/StringUtils.h>

#include <QFileDialog>
#include <QPointer>
#include <QStandardPaths>

namespace Ladybird {

BookmarksPage::BookmarksPage(u64 web_ui_id)
    : WebUIPage(web_ui_id)
    , BookmarkStoreObserver(Application::bookmark_store())
{
}

void BookmarksPage::received_message(String const& name, JsonValue const& data)
{
    if (name == "loadBookmarks"sv)
        load_bookmarks();
    else if (name == "moveItem"sv)
        move_item(data);
    else if (name == "newFolder"sv)
        new_folder(data);
    else if (name == "showContextMenu"sv)
        show_context_menu(data);
    else if (name == "importBookmarks"sv)
        import_bookmarks(data);
    else if (name == "exportBookmarks"sv)
        export_bookmarks(data);
    else
        warnln("Received message from about:bookmarks for unrecognized interface: {}", name);
}

void BookmarksPage::bookmarks_changed()
{
    load_bookmarks();
}

void BookmarksPage::load_bookmarks()
{
    auto items = Application::bookmark_store().serialize_items([](String const& favicon_hash) {
        return Application::bookmarks().favicon_png(favicon_hash);
    });
    send_message("loadBookmarks"_string, move(items));
}

void BookmarksPage::move_item(JsonValue const& data)
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

void BookmarksPage::new_folder(JsonValue const& data)
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

void BookmarksPage::import_bookmarks(JsonValue const& data)
{
    if (!data.is_array())
        return;

    Application::bookmark_store().import_items(data.as_array());
}

void BookmarksPage::export_bookmarks(JsonValue const& data)
{
    if (!data.is_string())
        return;

    auto* dialog = new QFileDialog(nullptr, "Select save location", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->selectFile("bookmarks.html");

    QObject::connect(dialog, &QDialog::finished, [bookmarks = data.as_string(), dialog = QPointer { dialog }](auto result) {
        if (result != QDialog::Accepted || !dialog || dialog->selectedFiles().isEmpty())
            return;

        auto destination = ak_byte_string_from_qstring(dialog->selectedFiles().first());
        auto file = Core::File::open(destination, Core::File::OpenMode::Write);
        if (file.is_error()) {
            Application::the().display_error_dialog(MUST(String::formatted("Unable to export bookmarks: {}", file.error())));
            return;
        }

        if (auto result = file.value()->write_until_depleted(bookmarks.bytes()); result.is_error())
            Application::the().display_error_dialog(MUST(String::formatted("Unable to export bookmarks: {}", result.error())));
    });
    dialog->open();
}

void BookmarksPage::show_context_menu(JsonValue const& data)
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
