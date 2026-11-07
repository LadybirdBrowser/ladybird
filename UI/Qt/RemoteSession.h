/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Forward.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/SessionStore.h>

namespace Ladybird {

// What the UI asks of the session of its windows, whose store lives in the Hub.
class RemoteSession {
public:
    static RemoteSession& of(WebView::IsPrivate);

    WebView::IsPrivate is_private() const { return m_is_private; }

    ErrorOr<WebView::SessionWindowId> window_opened();
    ErrorOr<void> window_closing(WebView::SessionStore::WindowClosing);
    void window_activated(WebView::SessionWindowId);
    void window_geometry_changed(WebView::SessionWindowId, WebView::SessionWindowGeometry);
    ErrorOr<WebView::SessionTabId> tab_opened(WebView::SessionStore::TabOpened);
    ErrorOr<void> tab_closed(WebView::SessionStore::TabClosed);
    void tab_detached(WebView::SessionTabId);
    void tab_moved(WebView::SessionStore::TabMoved);
    void tab_order_changed(WebView::SessionStore::TabOrderChanged);
    ErrorOr<void> tab_pinned_changed(WebView::SessionTabId, bool pinned);
    void active_tab_changed(WebView::SessionTabId);
    void application_quitting();
    void application_quit_aborted();
    u32 consecutive_unclean_exits() const;

    bool has_closed_units() const;
    ErrorOr<Optional<WebView::ClosedSessionUnit>> take_most_recently_closed();
    Vector<WebView::RecentlyClosedSessionUnit> recently_closed_units(size_t limit) const;
    ErrorOr<Optional<WebView::ClosedSessionUnit>> take_closed_unit(WebView::SessionWindowId);
    Vector<WebView::RestoredSessionWindow> take_launch_plan();
    Optional<WebView::SessionHistorySnapshot> load_tab_history(WebView::SessionTabId);

    Optional<WebView::HistoryEntry> entry_for_url(URL::URL const&) const;

private:
    explicit RemoteSession(WebView::IsPrivate is_private)
        : m_is_private(is_private)
    {
    }

    WebView::IsPrivate m_is_private { WebView::IsPrivate::No };
};

}
