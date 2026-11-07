/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/InternalURLs.h>
#include <LibURL/URL.h>
#include <LibWebView/AutocompleteRanker.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Bookmarks.h>
#include <UI/Qt/BookmarksPage.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/Tab.h>

#ifndef LADYBIRD_HUB_IN_OWN_PROCESS
#    include <LibWebView/Application.h>
#    include <LibWebView/FaviconStore.h>
#endif

namespace Ladybird {

Bookmarks::Bookmarks(BookmarkStore& store)
    : BookmarkStoreObserver(store)
    , WebView::SettingsObserver(Application::settings())
    , m_store(store)
{
    read_missing_favicons();
    update_autocomplete_bookmarks();
    create_menus();
    create_context_menus();

    if (auto* hub = HubConnection::the_if_connected())
        hub->async_add_web_ui_page("bookmarks"_string, "Bookmarks"_string);

    HashTable<String> favicon_hashes;
    for (auto const& favicon_hash : m_store.favicon_hashes())
        favicon_hashes.set(favicon_hash);
    Application::the().remove_unreferenced_favicons(move(favicon_hashes));
}

Bookmarks::~Bookmarks() = default;

void Bookmarks::page_opened(u64 web_ui_id)
{
    m_pages.set(web_ui_id, make<BookmarksPage>(web_ui_id));
}

void Bookmarks::page_received_message(u64 web_ui_id, String const& name, JsonValue const& data)
{
    if (auto* page = m_pages.get(web_ui_id).value_or(nullptr))
        page->received_message(name, data);
}

void Bookmarks::page_closed(u64 web_ui_id)
{
    m_pages.remove(web_ui_id);
}

void Bookmarks::bookmarks_changed()
{
    read_missing_favicons();
    update_autocomplete_bookmarks();

    m_menu->shrink(m_menu_static_size);
    create_menu_items();
    Application::the().rebuild_bookmarks_menu();
    update_toggle_bookmark_action();
}

void Bookmarks::appearance_changed()
{
    update_show_bookmarks_bar_actions();
}

void Bookmarks::create_menus()
{
    m_menu = WebView::Menu::create("Bookmarks"sv);
    m_menu->add_action(WebView::Action::create("Manage Bookmarks"sv, WebView::ActionID::ManageBookmarks, [] {
        Application::the().activate_or_open_tab_with_url(URL::about_bookmarks());
    }));

    auto add_show_bookmarks_bar_option = [&](auto name, auto value) {
        auto action = WebView::Action::create_checkable(name, WebView::ActionID::ShowBookmarksBar, [value] {
            auto appearance = Application::settings().appearance();
            appearance.show_bookmarks_bar = value;
            Application::settings().set_appearance(appearance);
        });
        action->set_checked(Application::settings().appearance().show_bookmarks_bar == value);
        m_show_bookmarks_bar_menu->add_action(action);
        return action;
    };

    m_show_bookmarks_bar_menu = WebView::Menu::create_group("Show Bookmarks Bar"sv);
    m_show_bookmarks_bar_always_action = add_show_bookmarks_bar_option("Always"sv, WebView::ShowBookmarksBar::Always);
    m_show_bookmarks_bar_never_action = add_show_bookmarks_bar_option("Never"sv, WebView::ShowBookmarksBar::Never);
    m_show_bookmarks_bar_on_hover_action = add_show_bookmarks_bar_option("Only When Hovering the Navigation Toolbar"sv, WebView::ShowBookmarksBar::OnHover);
    m_menu->add_submenu(*m_show_bookmarks_bar_menu);
    m_menu->add_separator();

    m_toggle_bookmark_action = WebView::Action::create("Toggle Bookmark"sv, WebView::ActionID::ToggleBookmark, [] {
        Application::the().show_bookmark_popover();
    });
    m_menu->add_action(*m_toggle_bookmark_action);
    update_toggle_bookmark_action();

    m_menu->add_action(WebView::Action::create("Bookmark All Tabs…"sv, WebView::ActionID::AddBookmarkAllTabs, [this] {
        auto bookmarks = Application::the().bookmarks_for_all_tabs_in_active_window();
        if (bookmarks.is_empty())
            return;

        auto default_title = MUST(UnixDateTime::now().to_string("Saved Tabs %Y-%m-%d"sv));
        Application::the().display_add_bookmark_folder_dialog(default_title)->when_resolved([this, bookmarks = move(bookmarks)](Application::BookmarkDialogResult result) mutable {
            auto& folder = result.data.get<BookmarkItem::Folder>();
            auto folder_id = m_store.add_folder(move(folder.title), result.target_folder_id);

            for (auto& bookmark : bookmarks)
                m_store.add_bookmark(move(bookmark.url), move(bookmark.title), move(bookmark.favicon_hash), folder_id);
        });
    }));

    m_toggle_bookmark_bar_action = WebView::Action::create("Show Bookmarks Bar"sv, WebView::ActionID::ToggleBookmarksBar, [] {
        auto appearance = Application::settings().appearance();
        appearance.show_bookmarks_bar = appearance.show_bookmarks_bar == WebView::ShowBookmarksBar::Always
            ? WebView::ShowBookmarksBar::Never
            : WebView::ShowBookmarksBar::Always;
        Application::settings().set_appearance(appearance);
    });

    m_menu->add_separator();
    m_menu_static_size = m_menu->size();
    create_menu_items();
}

void Bookmarks::create_context_menus()
{
    auto add_bookmark_action = WebView::Action::create("Add Bookmark…"sv, WebView::ActionID::AddBookmark, [this] {
        if (!m_context_menu_item.has_value())
            return;

        Application::the().display_add_bookmark_dialog(m_context_menu_item->target_folder_id)->when_resolved([this](Application::BookmarkDialogResult result) {
            auto& bookmark = result.data.get<BookmarkItem::Bookmark>();
            m_store.add_bookmark(move(bookmark.url), move(bookmark.title), move(bookmark.favicon_hash), result.target_folder_id);
        });
    });
    auto add_bookmark_folder_action = WebView::Action::create("New Folder…"sv, WebView::ActionID::AddBookmarkFolder, [this] {
        if (!m_context_menu_item.has_value())
            return;

        Application::the().display_add_bookmark_folder_dialog({}, m_context_menu_item->target_folder_id)->when_resolved([this](Application::BookmarkDialogResult result) {
            auto& folder = result.data.get<BookmarkItem::Folder>();
            m_store.add_folder(move(folder.title), result.target_folder_id);
        });
    });

    m_bookmarks_bar_context_menu = WebView::Menu::create("Bookmarks Bar Context Menu"sv);
    m_bookmarks_bar_context_menu->add_action(add_bookmark_action);
    m_bookmarks_bar_context_menu->add_action(add_bookmark_folder_action);
    m_bookmarks_bar_context_menu->add_separator();
    m_bookmarks_bar_context_menu->add_submenu(*m_show_bookmarks_bar_menu);

    m_bookmark_context_menu = WebView::Menu::create("Bookmark Context Menu"sv);
    m_bookmark_context_menu->add_action(WebView::Action::create("Open in New Tab"sv, WebView::ActionID::OpenInNewTab, [this] {
        if (m_context_menu_item.has_value())
            open_in_new_tab(m_context_menu_item->id, Web::HTML::ActivateTab::Yes);
    }));
    m_open_in_new_window_action = WebView::Action::create("Open in New Window"sv, WebView::ActionID::OpenInNewWindow, [this] {
        if (m_context_menu_item.has_value())
            open_url_in_new_window(m_context_menu_item->id, WebView::IsPrivate::No);
    });
    m_bookmark_context_menu->add_action(*m_open_in_new_window_action);
    m_bookmark_context_menu->add_action(WebView::Action::create("Open in Private Window"sv, WebView::ActionID::OpenInNewPrivateWindow, [this] {
        if (m_context_menu_item.has_value())
            open_url_in_new_window(m_context_menu_item->id, WebView::IsPrivate::Yes);
    }));
    m_bookmark_context_menu->add_separator();
    m_bookmark_context_menu->add_action(WebView::Action::create("Edit…"sv, WebView::ActionID::EditBookmark, [this] {
        if (!m_context_menu_item.has_value())
            return;

        auto current_bookmark = m_store.find_item_by_id(m_context_menu_item->id);
        if (!current_bookmark.has_value() || !current_bookmark->is_bookmark())
            return;

        Application::the().display_edit_bookmark_dialog(*current_bookmark, m_context_menu_item->parent_folder_id)->when_resolved([this, item = *m_context_menu_item](Application::BookmarkDialogResult result) {
            auto& bookmark = result.data.get<BookmarkItem::Bookmark>();
            m_store.edit_bookmark(item.id, move(bookmark.url), move(bookmark.title));
            if (item.parent_folder_id != result.target_folder_id)
                m_store.move_item(item.id, result.target_folder_id, NumericLimits<size_t>::max());
        });
    }));
    m_bookmark_context_menu->add_action(WebView::Action::create("Copy Link"sv, WebView::ActionID::CopyURL, [this] {
        if (!m_context_menu_item.has_value())
            return;

        auto bookmark = m_store.find_item_by_id(m_context_menu_item->id);
        if (!bookmark.has_value() || !bookmark->is_bookmark())
            return;

        Application::the().set_clipboard_text(MUST(String::from_byte_string(WebView::url_text_to_copy(bookmark->bookmark().url))));
    }));
    m_bookmark_context_menu->add_action(WebView::Action::create("Delete"sv, WebView::ActionID::DeleteBookmark, [this] {
        if (m_context_menu_item.has_value())
            m_store.remove_item(m_context_menu_item->id);
    }));
    m_bookmark_context_menu->add_separator();
    m_bookmark_context_menu->add_action(add_bookmark_folder_action);
    m_bookmark_context_menu->add_separator();
    m_bookmark_context_menu->add_submenu(*m_show_bookmarks_bar_menu);

    m_bookmark_folder_context_menu = WebView::Menu::create("Bookmark Folder Context Menu"sv);
    m_bookmark_folder_context_menu->add_action(WebView::Action::create("Open All in Tabs"sv, WebView::ActionID::OpenAllBookmarksInTabs, [this] {
        if (m_context_menu_item.has_value())
            open_folder_in_new_tabs(m_context_menu_item->id);
    }));
    m_bookmark_folder_context_menu->add_separator();
    m_bookmark_folder_context_menu->add_action(WebView::Action::create("Edit Folder…"sv, WebView::ActionID::EditBookmarkFolder, [this] {
        if (!m_context_menu_item.has_value())
            return;

        auto current_folder = m_store.find_item_by_id(m_context_menu_item->id);
        if (!current_folder.has_value() || !current_folder->is_folder())
            return;

        Application::the().display_edit_bookmark_folder_dialog(*current_folder, m_context_menu_item->parent_folder_id)->when_resolved([this, item = *m_context_menu_item](Application::BookmarkDialogResult result) {
            auto& folder = result.data.get<BookmarkItem::Folder>();
            m_store.edit_folder(item.id, move(folder.title));
            if (item.parent_folder_id != result.target_folder_id)
                m_store.move_item(item.id, result.target_folder_id, NumericLimits<size_t>::max());
        });
    }));
    m_bookmark_folder_context_menu->add_action(WebView::Action::create("Delete Folder"sv, WebView::ActionID::DeleteBookmarkFolder, [this] {
        if (m_context_menu_item.has_value())
            m_store.remove_item(m_context_menu_item->id);
    }));
    m_bookmark_folder_context_menu->add_separator();
    m_bookmark_folder_context_menu->add_action(add_bookmark_action);
    m_bookmark_folder_context_menu->add_action(add_bookmark_folder_action);
    m_bookmark_folder_context_menu->add_separator();
    m_bookmark_folder_context_menu->add_submenu(*m_show_bookmarks_bar_menu);
}

void Bookmarks::create_menu_items(Optional<MenuData> data)
{
    auto const& [menu, items, target_folder_id] = data.ensure([&]() -> MenuData {
        return {
            .menu = *m_menu,
            .items = m_store.root_items(),
            .target_folder_id = {},
        };
    });

    for (auto const& item : items) {
        item.data.visit(
            [&](BookmarkItem::Bookmark const& bookmark) {
                auto action = WebView::Action::create(bookmark.title.value_or({}), WebView::ActionID::BookmarkItem, [this, url = bookmark.url] {
                    open_url(url);
                });

                action->set_png_icon(bookmark.favicon_hash.has_value() ? favicon_png(*bookmark.favicon_hash) : OptionalNone {});
                action->set_tooltip(bookmark.url.serialize());

                action->add_property("id"sv, item.id);
                action->add_property("type"sv, "bookmark"_string);
                if (target_folder_id.has_value()) {
                    action->add_property("target_folder_id"sv, *target_folder_id);
                    action->add_property("parent_folder_id"sv, *target_folder_id);
                }

                menu.add_action(move(action));
            },
            [&](BookmarkItem::Folder const& folder) {
                auto submenu = folder.title.has_value()
                    ? WebView::Menu::create_group(*folder.title)
                    : WebView::Menu::create_group("(no title)"sv);

                create_menu_items(MenuData {
                    .menu = submenu,
                    .items = folder.children,
                    .target_folder_id = item.id,
                });

                // A folder's menu ends with a way to open every bookmark in it at once.
                size_t bookmark_count = 0;
                for (auto const& child : folder.children)
                    bookmark_count += child.is_bookmark() ? 1 : 0;
                if (bookmark_count > 0) {
                    submenu->add_separator();
                    submenu->add_action(WebView::Action::create(MUST(String::formatted("Open All in Tabs ({})", bookmark_count)), WebView::ActionID::OpenAllBookmarksInTabs, [this, folder_id = item.id] {
                        open_folder_in_new_tabs(folder_id);
                    }));
                }

                submenu->add_property("id"sv, item.id);
                submenu->add_property("type"sv, "folder"_string);
                submenu->add_property("target_folder_id"sv, item.id);
                if (target_folder_id.has_value())
                    submenu->add_property("parent_folder_id"sv, *target_folder_id);

                submenu->set_render_group_icon(true);
                menu.add_submenu(move(submenu));
            });
    }
}

void Bookmarks::update_show_bookmarks_bar_actions()
{
    switch (Application::settings().appearance().show_bookmarks_bar) {
    case WebView::ShowBookmarksBar::Always:
        m_show_bookmarks_bar_always_action->set_checked(true);
        break;
    case WebView::ShowBookmarksBar::Never:
        m_show_bookmarks_bar_never_action->set_checked(true);
        break;
    case WebView::ShowBookmarksBar::OnHover:
        m_show_bookmarks_bar_on_hover_action->set_checked(true);
        break;
    }
}

void Bookmarks::update_toggle_bookmark_action()
{
    auto* tab = Application::the().active_tab();
    auto is_bookmarked = tab && m_store.is_bookmarked(tab->view().url());

    m_toggle_bookmark_action->set_text(is_bookmarked ? "Remove Bookmark"sv : "Add Bookmark"sv);
    m_toggle_bookmark_action->set_engaged(is_bookmarked);
}

void Bookmarks::set_context_menu_item(Optional<String> item_id, Optional<String> target_folder_id, Optional<String> parent_folder_id)
{
    // NB: A private window does not offer to leave for a normal one.
    auto* tab = Application::the().active_tab();
    m_open_in_new_window_action->set_visible(!tab || tab->view().is_private() == WebView::IsPrivate::No);

    if (!item_id.has_value() && !target_folder_id.has_value()) {
        m_context_menu_item = {};
        return;
    }
    m_context_menu_item = ContextMenuItem {
        .id = item_id.value_or({}),
        .target_folder_id = move(target_folder_id),
        .parent_folder_id = move(parent_folder_id),
    };
}

void Bookmarks::open_url(URL::URL const& url) const
{
    if (auto* tab = Application::the().active_tab())
        tab->view().load_from_user_input(url);
    else
        open_url_in_new_tab(url, Web::HTML::ActivateTab::Yes);
}

void Bookmarks::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab) const
{
    if (auto* tab = Application::the().active_tab())
        tab->view().open_url_in_new_tab(url, activate_tab);
    else
        Application::the().open_url_in_new_tab(url, activate_tab, {}, WebView::IsPrivate::No);
}

void Bookmarks::open_in_new_tab(String const& bookmark_id, Web::HTML::ActivateTab activate_tab)
{
    if (auto bookmark = m_store.find_item_by_id(bookmark_id); bookmark.has_value() && bookmark->is_bookmark())
        open_url_in_new_tab(bookmark->bookmark().url, activate_tab);
}

void Bookmarks::open_url_in_new_window(String const& bookmark_id, WebView::IsPrivate is_private) const
{
    auto bookmark = m_store.find_item_by_id(bookmark_id);
    if (!bookmark.has_value() || !bookmark->is_bookmark())
        return;

    if (auto* tab = Application::the().active_tab())
        tab->view().open_url_in_new_window(bookmark->bookmark().url, is_private);
    else
        Application::the().open_url_in_new_window(bookmark->bookmark().url, is_private);
}

static void collect_bookmark_urls(BookmarkItem::Folder const& folder, Vector<URL::URL>& urls)
{
    for (auto const& child : folder.children) {
        child.data.visit(
            [&](BookmarkItem::Bookmark const& bookmark) {
                urls.append(bookmark.url);
            },
            [&](BookmarkItem::Folder const& child_folder) {
                collect_bookmark_urls(child_folder, urls);
            });
    }
}

void Bookmarks::open_folder_in_new_tabs(String const& folder_id) const
{
    auto folder = m_store.find_item_by_id(folder_id);
    if (!folder.has_value() || !folder->is_folder())
        return;

    Vector<URL::URL> urls;
    collect_bookmark_urls(folder->folder(), urls);

    // NB: URLs that another application handles go through the view, which knows how to ask about that.
    Vector<URL::URL> internal_urls;
    auto* tab = Application::the().active_tab();
    for (auto const& url : urls) {
        if (tab && !WebView::is_url_handled_internally(url))
            tab->view().open_url_in_new_tab(url, Web::HTML::ActivateTab::No);
        else
            internal_urls.append(url);
    }

    Application::the().open_urls_in_new_tabs(internal_urls);
}

void Bookmarks::read_missing_favicons()
{
    Vector<String> missing_hashes;
    for (auto const& hash : m_store.favicon_hashes()) {
        if (!m_favicons.contains(hash))
            missing_hashes.append(hash);
    }
    if (missing_hashes.is_empty())
        return;

    for (auto const& hash : missing_hashes)
        m_favicons.set(hash, Application::favicon_store(WebView::IsPrivate::No).favicon_png(hash));
}

Optional<ByteBuffer> Bookmarks::favicon_png(String const& favicon_hash) const
{
    return m_favicons.get(favicon_hash).value_or({});
}

static void append_autocomplete_bookmarks(Bookmarks const& bookmarks, Vector<WebView::AutocompleteBookmark>& autocomplete_bookmarks, ReadonlySpan<BookmarkItem> items, Optional<String> const& parent_folder)
{
    for (auto const& item : items) {
        if (item.is_bookmark()) {
            auto const& bookmark = item.bookmark();
            autocomplete_bookmarks.append({
                .url = bookmark.url.serialize(URL::ExcludeFragment::Yes),
                .title = bookmark.title,
                .folder = parent_folder,
                .favicon_png = bookmark.favicon_hash.has_value() ? bookmarks.favicon_png(*bookmark.favicon_hash) : OptionalNone {},
            });
            continue;
        }

        auto folder = parent_folder;
        if (item.folder().title.has_value() && !item.folder().title->is_empty()) {
            folder = parent_folder.has_value()
                ? MUST(String::formatted("{} / {}", *parent_folder, *item.folder().title))
                : *item.folder().title;
        }
        append_autocomplete_bookmarks(bookmarks, autocomplete_bookmarks, item.folder().children, folder);
    }
}

void Bookmarks::update_autocomplete_bookmarks()
{
    m_autocomplete_bookmarks.clear();
    append_autocomplete_bookmarks(*this, m_autocomplete_bookmarks, m_store.root_items(), {});
}

Vector<WebView::AutocompleteSuggestion> Bookmarks::suggestions_for(StringView query, size_t max_suggestions) const
{
    return WebView::rank_bookmark_suggestions(query.trim_whitespace(), m_autocomplete_bookmarks, WebView::autocomplete_preliminary_limit(max_suggestions));
}

}
