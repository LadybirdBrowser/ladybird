/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <UI/Qt/BookmarkStore.h>
#include <UI/Qt/WebUIPage.h>

namespace Ladybird {

class BookmarksPage final
    : public WebUIPage
    , public BookmarkStoreObserver {
public:
    explicit BookmarksPage(u64 web_ui_id);

    virtual void received_message(String const& name, JsonValue const& data) override;

private:
    virtual void bookmarks_changed() override;

    void load_bookmarks();
    void move_item(JsonValue const&);
    void new_folder(JsonValue const&);
    void import_bookmarks(JsonValue const&);
    void export_bookmarks(JsonValue const&);
    void show_context_menu(JsonValue const&);
};

}
