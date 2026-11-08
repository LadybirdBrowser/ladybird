/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/HTML/ActivateTab.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Actions.h>
#include <UI/Qt/BookmarkStore.h>

namespace Ladybird {

// Everything about bookmarks: they are a feature of this application, of which the Hub knows nothing.
class Bookmarks final
    : public BookmarkStoreObserver
    , public WebView::SettingsObserver {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit Bookmarks(BookmarkStore&);
    virtual ~Bookmarks() override;

    Vector<WebView::AutocompleteSuggestion> suggestions_for(StringView query, size_t max_suggestions) const;
    Optional<ByteBuffer> favicon_png(String const& favicon_hash) const;

    Menu& menu() { return *m_menu; }
    Menu& show_bookmarks_bar_menu() { return *m_show_bookmarks_bar_menu; }
    Menu& bookmarks_bar_context_menu() { return *m_bookmarks_bar_context_menu; }
    Menu& bookmark_context_menu() { return *m_bookmark_context_menu; }
    Menu& bookmark_folder_context_menu() { return *m_bookmark_folder_context_menu; }
    Action& toggle_bookmark_bar_action() { return *m_toggle_bookmark_bar_action; }

    void update_toggle_bookmark_action();
    void set_context_menu_item(Optional<String> item_id, Optional<String> target_folder_id, Optional<String> parent_folder_id);
    void open_in_new_tab(String const& bookmark_id, Web::HTML::ActivateTab);

private:
    virtual void bookmarks_changed() override;

    virtual void appearance_changed() override;

    struct ContextMenuItem {
        String id;
        Optional<String> target_folder_id;
        Optional<String> parent_folder_id;
    };

    struct MenuData {
        Menu& menu;
        ReadonlySpan<BookmarkItem> items;
        Optional<String const&> target_folder_id;
    };

    void create_menus();
    void create_context_menus();
    void create_menu_items(Optional<MenuData> = {});
    void update_show_bookmarks_bar_actions();

    void open_url(URL::URL const&) const;
    void open_url_in_new_tab(URL::URL const&, Web::HTML::ActivateTab) const;
    void open_url_in_new_window(String const& bookmark_id, WebView::IsPrivate) const;
    void open_folder_in_new_tabs(String const& folder_id) const;

    void read_missing_favicons();
    void update_autocomplete_bookmarks();

    BookmarkStore& m_store;

    RefPtr<Menu> m_menu;
    RefPtr<Menu> m_show_bookmarks_bar_menu;
    RefPtr<Menu> m_bookmarks_bar_context_menu;
    RefPtr<Menu> m_bookmark_context_menu;
    RefPtr<Menu> m_bookmark_folder_context_menu;
    RefPtr<Action> m_open_in_new_window_action;
    RefPtr<Action> m_toggle_bookmark_action;
    RefPtr<Action> m_toggle_bookmark_bar_action;
    RefPtr<Action> m_show_bookmarks_bar_always_action;
    RefPtr<Action> m_show_bookmarks_bar_never_action;
    RefPtr<Action> m_show_bookmarks_bar_on_hover_action;
    size_t m_menu_static_size { 0 };
    Optional<ContextMenuItem> m_context_menu_item;

    // A favicon's hash is of its content, so what was read for a hash stays true.
    HashMap<String, Optional<ByteBuffer>> m_favicons;
    Vector<WebView::AutocompleteBookmark> m_autocomplete_bookmarks;
};

}
