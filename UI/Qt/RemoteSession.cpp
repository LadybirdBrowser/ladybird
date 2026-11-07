/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <UI/Qt/RemoteSession.h>

namespace Ladybird {

RemoteSession& RemoteSession::of(WebView::IsPrivate is_private)
{
    static RemoteSession s_regular { WebView::IsPrivate::No };
    static RemoteSession s_private { WebView::IsPrivate::Yes };
    return is_private == WebView::IsPrivate::Yes ? s_private : s_regular;
}

WebView::SessionStore& RemoteSession::store() const
{
    return *WebView::Application::session_for_new_view(m_is_private)->session_store;
}

ErrorOr<WebView::SessionWindowId> RemoteSession::window_opened() { return store().window_opened(); }
ErrorOr<void> RemoteSession::window_closing(WebView::SessionStore::WindowClosing closing) { return store().window_closing(move(closing)); }
void RemoteSession::window_activated(WebView::SessionWindowId id) { store().window_activated(id); }
void RemoteSession::window_geometry_changed(WebView::SessionWindowId id, WebView::SessionWindowGeometry geometry) { store().window_geometry_changed(id, geometry); }
ErrorOr<WebView::SessionTabId> RemoteSession::tab_opened(WebView::SessionStore::TabOpened opened) { return store().tab_opened(move(opened)); }
ErrorOr<void> RemoteSession::tab_closed(WebView::SessionStore::TabClosed closed) { return store().tab_closed(move(closed)); }
void RemoteSession::tab_detached(WebView::SessionTabId id) { store().tab_detached(id); }
void RemoteSession::tab_moved(WebView::SessionStore::TabMoved moved) { store().tab_moved(move(moved)); }
void RemoteSession::tab_order_changed(WebView::SessionStore::TabOrderChanged changed) { store().tab_order_changed(move(changed)); }
ErrorOr<void> RemoteSession::tab_pinned_changed(WebView::SessionTabId id, bool pinned) { return store().tab_pinned_changed(id, pinned); }
void RemoteSession::active_tab_changed(WebView::SessionTabId id) { store().active_tab_changed(id); }
void RemoteSession::application_quitting() { store().application_quitting(); }
void RemoteSession::application_quit_aborted() { store().application_quit_aborted(); }
u32 RemoteSession::consecutive_unclean_exits() const { return store().consecutive_unclean_exits(); }
bool RemoteSession::has_closed_units() const { return store().has_closed_units(); }
ErrorOr<Optional<WebView::ClosedSessionUnit>> RemoteSession::take_most_recently_closed() { return store().take_most_recently_closed(); }
Vector<WebView::RecentlyClosedSessionUnit> RemoteSession::recently_closed_units(size_t limit) const { return store().recently_closed_units(limit); }
ErrorOr<Optional<WebView::ClosedSessionUnit>> RemoteSession::take_closed_unit(WebView::SessionWindowId id) { return store().take_closed_unit(id); }
Vector<WebView::RestoredSessionWindow> RemoteSession::take_launch_plan() { return store().take_launch_plan(); }
Optional<WebView::SessionHistorySnapshot> RemoteSession::load_tab_history(WebView::SessionTabId id) { return store().load_tab_history(id); }

Optional<WebView::HistoryEntry> RemoteSession::entry_for_url(URL::URL const& url) const
{
    return WebView::Application::session_for_new_view(m_is_private)->history_store->entry_for_url(url);
}

}
