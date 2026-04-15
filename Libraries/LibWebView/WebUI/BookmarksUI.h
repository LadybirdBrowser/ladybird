/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/BookmarkStore.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class BookmarksUI
    : public WebUI
    , public BookmarkStoreObserver {
    WEB_UI(BookmarksUI);

private:
    virtual void register_interfaces() override;
    virtual void bookmarks_changed() override;

    void load_bookmarks();
    void move_item(JsonValue const&);
    void import_bookmarks(JsonValue const&);
    void export_bookmarks(JsonValue const&);
    void show_context_menu(JsonValue const&);
};

}
