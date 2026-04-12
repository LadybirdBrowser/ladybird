/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Random.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/Utilities.h>

namespace WebView {

static constexpr auto VERSION_KEY = "version"sv;
static constexpr auto ITEMS_KEY = "items"sv;
static constexpr auto TYPE_KEY = "type"sv;
static constexpr auto ID_KEY = "id"sv;
static constexpr auto URL_KEY = "url"sv;
static constexpr auto TITLE_KEY = "title"sv;
static constexpr auto FAVICON_KEY = "favicon"sv;
static constexpr auto CHILDREN_KEY = "children"sv;
static constexpr auto DATE_ADDED_KEY = "dateAdded"sv;
static constexpr auto LAST_MODIFIED_KEY = "lastModified"sv;

static constexpr auto TYPE_BOOKMARK = "bookmark"sv;
static constexpr auto TYPE_FOLDER = "folder"sv;

static Vector<BookmarkItem> parse_bookmark_items(JsonArray const& array)
{
    Vector<BookmarkItem> items;
    items.ensure_capacity(array.size());

    array.for_each([&](JsonValue const& value) {
        if (!value.is_object())
            return;
        auto const& object = value.as_object();

        auto id = object.get_string(ID_KEY);
        if (!id.has_value())
            return;

        auto type = object.get_string(TYPE_KEY);
        auto title = object.get_string(TITLE_KEY);

        auto date_added = UnixDateTime {};
        if (auto date_added_ms = object.get_integer<i64>(DATE_ADDED_KEY); date_added_ms.has_value())
            date_added = UnixDateTime::from_milliseconds_since_epoch(*date_added_ms);

        auto last_modified = UnixDateTime {};
        if (auto last_modified_ms = object.get_integer<i64>(LAST_MODIFIED_KEY); last_modified_ms.has_value())
            last_modified = UnixDateTime::from_milliseconds_since_epoch(*last_modified_ms);

        if (type == TYPE_BOOKMARK) {
            auto url_string = object.get_string(URL_KEY);
            if (!url_string.has_value())
                return;

            auto url = URL::Parser::basic_parse(*url_string);
            if (!url.has_value())
                return;

            auto favicon = object.get_string(FAVICON_KEY);

            items.empend(
                id.release_value(),
                date_added,
                last_modified,
                BookmarkItem::Bookmark {
                    .url = url.release_value(),
                    .title = title.map([](auto title) { return title; }),
                    .favicon_base64_png = favicon.map([](auto favicon) { return favicon; }),
                });
        } else if (type == TYPE_FOLDER) {
            Vector<BookmarkItem> children;
            if (auto children_array = object.get_array(CHILDREN_KEY); children_array.has_value())
                children = parse_bookmark_items(*children_array);

            items.empend(
                id.release_value(),
                date_added,
                last_modified,
                BookmarkItem::Folder {
                    .title = title.map([](auto title) { return title; }),
                    .children = move(children),
                });
        }
    });

    return items;
}

static JsonObject serialize_bookmark_item(BookmarkItem const& item)
{
    JsonObject object;
    object.set(ID_KEY, item.id);
    object.set(DATE_ADDED_KEY, item.date_added.milliseconds_since_epoch());
    object.set(LAST_MODIFIED_KEY, item.last_modified.milliseconds_since_epoch());

    item.data.visit(
        [&](BookmarkItem::Bookmark const& bookmark) {
            object.set(TYPE_KEY, TYPE_BOOKMARK);
            object.set(URL_KEY, bookmark.url.serialize());

            if (bookmark.title.has_value())
                object.set(TITLE_KEY, *bookmark.title);

            if (bookmark.favicon_base64_png.has_value())
                object.set(FAVICON_KEY, *bookmark.favicon_base64_png);
        },
        [&](BookmarkItem::Folder const& folder) {
            object.set(TYPE_KEY, TYPE_FOLDER);

            if (folder.title.has_value())
                object.set(TITLE_KEY, *folder.title);

            JsonArray children;
            children.ensure_capacity(folder.children.size());

            for (auto const& child : folder.children)
                children.must_append(serialize_bookmark_item(child));

            object.set(CHILDREN_KEY, move(children));
        });

    return object;
}

static Vector<BookmarkItem> create_default_bookmarks()
{
    auto now = UnixDateTime::now();

    return {
        {
            .id = generate_random_uuid(),
            .date_added = now,
            .last_modified = now,
            .data = BookmarkItem::Bookmark {
                .url = URL::Parser::basic_parse("https://ladybird.org/"sv).release_value(),
                .title = "Ladybird"_string,
                .favicon_base64_png = {},
            },
        },
        {
            .id = generate_random_uuid(),
            .date_added = now,
            .last_modified = now,
            .data = BookmarkItem::Bookmark {
                .url = URL::Parser::basic_parse("https://github.com/LadybirdBrowser/ladybird"sv).release_value(),
                .title = "Ladybird GitHub"_string,
                .favicon_base64_png = {},
            },
        },
        {
            .id = generate_random_uuid(),
            .date_added = now,
            .last_modified = now,
            .data = BookmarkItem::Bookmark {
                .url = URL::Parser::basic_parse("https://discord.com/invite/nvfjVJ4Svh"sv).release_value(),
                .title = "Ladybird Discord"_string,
                .favicon_base64_png = {},
            },
        },
    };
}

BookmarkStore BookmarkStore::create(Badge<Application>)
{
    auto bookmarks_directory = ByteString::formatted("{}/Ladybird", Core::StandardPaths::config_directory());
    auto bookmarks_path = ByteString::formatted("{}/Bookmarks.json", bookmarks_directory);

    BookmarkStore store { move(bookmarks_path) };

    if (!FileSystem::exists(store.m_bookmarks_path)) {
        store.m_items = create_default_bookmarks();
        store.persist_bookmarks();
        return store;
    }

    auto bookmarks_json = read_json_file(store.m_bookmarks_path);
    if (bookmarks_json.is_error()) {
        warnln("Unable to read Ladybird bookmarks: {}", bookmarks_json.error());
        return store;
    }

    if (auto items = bookmarks_json.value().get_array(ITEMS_KEY); items.has_value())
        store.m_items = parse_bookmark_items(*items);

    return store;
}

BookmarkStore::BookmarkStore(ByteString bookmarks_path)
    : m_bookmarks_path(move(bookmarks_path))
{
}

bool BookmarkStore::is_bookmarked(URL::URL const& url) const
{
    return find_bookmark_by_url(url).has_value();
}

static Optional<BookmarkItem const&> find_bookmark_by_url_impl(ReadonlySpan<BookmarkItem> items, URL::URL const& url)
{
    for (auto const& item : items) {
        if (item.is_bookmark() && item.bookmark().url == url)
            return item;

        if (item.is_folder()) {
            if (auto found = find_bookmark_by_url_impl(item.folder().children, url); found.has_value())
                return found;
        }
    }

    return {};
}

Optional<BookmarkItem const&> BookmarkStore::find_bookmark_by_url(URL::URL const& url) const
{
    return find_bookmark_by_url_impl(m_items, url);
}

template<typename ListType>
static Optional<CopyConst<ListType, BookmarkItem>&> find_item_by_id_impl(ListType& items, StringView id)
{
    for (auto& item : items) {
        if (item.id == id)
            return item;

        if (item.is_folder()) {
            if (auto found = find_item_by_id_impl(item.folder().children, id); found.has_value())
                return found;
        }
    }

    return {};
}

Optional<BookmarkItem const&> BookmarkStore::BookmarkStore::find_item_by_id(StringView id) const
{
    return find_item_by_id_impl(m_items, id);
}

Optional<BookmarkItem&> BookmarkStore::BookmarkStore::find_mutable_item_by_id(StringView id)
{
    return find_item_by_id_impl(m_items, id);
}

static Vector<BookmarkItem>& find_target_folder(Vector<BookmarkItem>& items, Optional<String const&> target_folder_id)
{
    if (target_folder_id.has_value()) {
        if (auto target = find_item_by_id_impl(items, *target_folder_id); target.has_value() && target->is_folder())
            return target->folder().children;
    }

    return items;
}

void BookmarkStore::add_bookmark(URL::URL url, Optional<String> title, Optional<String> favicon_base64_png, Optional<String const&> target_folder_id)
{
    BookmarkItem item {
        .id = generate_random_uuid(),
        .date_added = UnixDateTime::now(),
        .last_modified = UnixDateTime::now(),
        .data = BookmarkItem::Bookmark {
            .url = move(url),
            .title = move(title),
            .favicon_base64_png = move(favicon_base64_png),
        },
    };

    find_target_folder(m_items, target_folder_id).append(move(item));

    persist_bookmarks();
    notify_observers();
}

void BookmarkStore::add_folder(Optional<String> title, Optional<String const&> target_folder_id)
{
    BookmarkItem item {
        .id = generate_random_uuid(),
        .date_added = UnixDateTime::now(),
        .last_modified = UnixDateTime::now(),
        .data = BookmarkItem::Folder {
            .title = move(title),
            .children = {},
        },
    };

    find_target_folder(m_items, target_folder_id).append(move(item));

    persist_bookmarks();
    notify_observers();
}

void BookmarkStore::edit_bookmark(StringView id, URL::URL url, Optional<String> title)
{
    auto item = find_mutable_item_by_id(id);
    if (!item.has_value() || !item->is_bookmark())
        return;

    auto& bookmark = item->bookmark();
    bookmark.url = move(url);
    bookmark.title = move(title);
    item->last_modified = UnixDateTime::now();

    persist_bookmarks();
    notify_observers();
}

void BookmarkStore::edit_folder(StringView id, Optional<String> title)
{
    auto item = find_mutable_item_by_id(id);
    if (!item.has_value() || !item->is_folder())
        return;

    item->folder().title = move(title);
    item->last_modified = UnixDateTime::now();

    persist_bookmarks();
    notify_observers();
}

void BookmarkStore::remove_item(StringView id)
{
    auto containing_item_list = find_containing_item_list(id);
    if (!containing_item_list.has_value())
        return;

    if (containing_item_list->remove_all_matching([&](auto const& item) { return item.id == id; })) {
        persist_bookmarks();
        notify_observers();
    }
}

static bool is_descendant_of(BookmarkItem const& ancestor, StringView descendant_id)
{
    if (!ancestor.is_folder())
        return false;

    for (auto const& child : ancestor.folder().children) {
        if (child.id == descendant_id)
            return true;
        if (is_descendant_of(child, descendant_id))
            return true;
    }

    return false;
}

void BookmarkStore::move_item(StringView id, Optional<String const&> target_folder_id, size_t index)
{
    if (target_folder_id.has_value()) {
        if (id == *target_folder_id)
            return;

        auto item = find_item_by_id(id);
        if (!item.has_value())
            return;

        // Disallow moving a folder to one of its own descendents.
        if (is_descendant_of(*item, *target_folder_id))
            return;
    }

    auto source_list = find_containing_item_list(id);
    if (!source_list.has_value())
        return;

    // Check if source and target are the same list before removal. We cannot hold a reference to the target list across
    // the removal, as that may shift elements in memory and invalidate the reference.
    bool same_list = &source_list.value() == &find_target_folder(m_items, target_folder_id);

    Optional<BookmarkItem> moved_item;
    Optional<size_t> item_index;

    for (auto [i, item] : enumerate(*source_list)) {
        if (item.id == id) {
            moved_item = move(item);
            item_index = i;

            source_list->remove(i);
            break;
        }
    }

    if (!moved_item.has_value())
        return;

    // When moving within the same list, the caller provides the index relative to the original list (before removal).
    // Adjust for the removed item if it was before the target position.
    if (same_list && *item_index < index && index > 0)
        --index;

    auto& target_list = find_target_folder(m_items, target_folder_id);
    index = min(index, target_list.size());

    target_list.insert(index, moved_item.release_value());
    target_list[index].last_modified = UnixDateTime::now();

    persist_bookmarks();
    notify_observers();
}

void BookmarkStore::update_favicon(URL::URL const& url, String favicon_base64_png)
{
    auto item = find_bookmark_by_url(url);
    if (!item.has_value() || !item->is_bookmark())
        return;

    auto& bookmark = const_cast<BookmarkItem::Bookmark&>(item->bookmark());

    if (bookmark.favicon_base64_png == favicon_base64_png)
        return;

    bookmark.favicon_base64_png = move(favicon_base64_png);
    const_cast<BookmarkItem&>(*item).last_modified = UnixDateTime::now();

    persist_bookmarks();
    notify_observers();
}

static Optional<Vector<BookmarkItem>&> find_containing_item_list_impl(Vector<BookmarkItem>& items, StringView id)
{
    for (auto& item : items) {
        if (!item.is_folder())
            continue;

        auto& children = item.folder().children;
        for (auto const& child : children) {
            if (child.id == id)
                return children;
        }

        if (auto found = find_containing_item_list_impl(children, id); found.has_value())
            return found;
    }

    return {};
}

Optional<Vector<BookmarkItem>&> BookmarkStore::find_containing_item_list(StringView id)
{
    for (auto const& item : m_items) {
        if (item.id == id)
            return m_items;
    }

    return find_containing_item_list_impl(m_items, id);
}

JsonValue BookmarkStore::serialize_items() const
{
    JsonArray items;
    items.ensure_capacity(m_items.size());

    for (auto const& item : m_items)
        items.must_append(serialize_bookmark_item(item));

    return items;
}

void BookmarkStore::persist_bookmarks()
{
    JsonObject root;
    root.set(VERSION_KEY, 1);
    root.set(ITEMS_KEY, serialize_items());

    if (auto result = write_json_file(m_bookmarks_path, root); result.is_error())
        warnln("Unable to persist Ladybird bookmarks: {}", result.error());
}

void BookmarkStore::notify_observers()
{
    for (auto& observer : m_observers)
        observer.bookmarks_changed();
}

void BookmarkStore::add_observer(Badge<BookmarkStoreObserver>, BookmarkStoreObserver& observer)
{
    Application::bookmark_store().m_observers.append(observer);
}

void BookmarkStore::remove_observer(Badge<BookmarkStoreObserver>, BookmarkStoreObserver& observer)
{
    auto was_removed = Application::bookmark_store().m_observers.remove_first_matching([&](auto const& candidate) {
        return &candidate == &observer;
    });
    VERIFY(was_removed);
}

BookmarkStoreObserver::BookmarkStoreObserver()
{
    BookmarkStore::add_observer({}, *this);
}

BookmarkStoreObserver::~BookmarkStoreObserver()
{
    BookmarkStore::remove_observer({}, *this);
}

}
