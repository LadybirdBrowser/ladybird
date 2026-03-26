/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API BookmarkItem {
    struct Bookmark {
        URL::URL url;
        Optional<String> title;
        Optional<String> favicon_base64_png;
    };

    struct Folder {
        Optional<String> title;
        Vector<BookmarkItem> children;
    };

    String id;
    Variant<Bookmark, Folder> data;

    bool is_bookmark() const { return data.has<Bookmark>(); }
    bool is_folder() const { return data.has<Folder>(); }

    Bookmark const& bookmark() const { return data.get<Bookmark>(); }
    Bookmark& bookmark() { return data.get<Bookmark>(); }

    Folder const& folder() const { return data.get<Folder>(); }
    Folder& folder() { return data.get<Folder>(); }
};

class WEBVIEW_API BookmarkStoreObserver {
public:
    explicit BookmarkStoreObserver();
    virtual ~BookmarkStoreObserver();

    virtual void bookmarks_changed() { }
};

class WEBVIEW_API BookmarkStore {
public:
    static BookmarkStore create(Badge<Application>);

    Vector<BookmarkItem> const& root_items() const { return m_items; }

    bool is_bookmarked(URL::URL const&) const;

    Optional<BookmarkItem const&> find_bookmark_by_url(URL::URL const&) const;
    Optional<BookmarkItem const&> find_item_by_id(StringView id) const;

    void add_bookmark(URL::URL url, Optional<String> title, Optional<String> favicon_base64, Optional<String const&> target_folder_id = {});
    void add_folder(Optional<String> title, Optional<String const&> target_folder_id = {});

    void edit_bookmark(StringView id, URL::URL url, Optional<String> title);
    void edit_folder(StringView id, Optional<String> title);

    void remove_item(StringView id);

    void update_favicon(URL::URL const& url, String favicon_base64);

    static void add_observer(Badge<BookmarkStoreObserver>, BookmarkStoreObserver&);
    static void remove_observer(Badge<BookmarkStoreObserver>, BookmarkStoreObserver&);

private:
    explicit BookmarkStore(ByteString bookmarks_path);

    Optional<BookmarkItem&> find_mutable_item_by_id(StringView id);
    Optional<Vector<BookmarkItem>&> find_containing_item_list(StringView id);

    void persist_bookmarks();
    void notify_observers();

    ByteString m_bookmarks_path;
    Vector<BookmarkItem> m_items;
    Vector<BookmarkStoreObserver&> m_observers;
};

}
