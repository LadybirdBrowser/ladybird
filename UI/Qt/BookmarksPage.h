/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <AK/String.h>
#include <UI/Qt/BookmarkStore.h>

namespace Ladybird {

// One open about:bookmarks. The page reaches this through the Hub, which does not know what the page is about.
class BookmarksPage final : public BookmarkStoreObserver {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit BookmarksPage(u64 web_ui_id);

    void received_message(String const& name, JsonValue const& data);

private:
    virtual void bookmarks_changed() override;

    void load_bookmarks();
    void move_item(JsonValue const&);
    void new_folder(JsonValue const&);
    void import_bookmarks(JsonValue const&);
    void export_bookmarks(JsonValue const&);
    void show_context_menu(JsonValue const&);

    u64 m_web_ui_id { 0 };
};

}
