/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/HubConnection.h>
#include <UI/Qt/RemoteSession.h>

#ifndef LADYBIRD_HUB_IN_OWN_PROCESS
#    include <LibWebView/Application.h>
#    include <LibWebView/BrowsingSession.h>
#endif

namespace Ladybird {

RemoteSession& RemoteSession::of(WebView::IsPrivate is_private)
{
    static RemoteSession s_regular { WebView::IsPrivate::No };
    static RemoteSession s_private { WebView::IsPrivate::Yes };
    return is_private == WebView::IsPrivate::Yes ? s_private : s_regular;
}

#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
static ErrorOr<void> error_unless(bool succeeded)
{
    if (!succeeded)
        return Error::from_string_literal("The session store could not do that");
    return {};
}

template<typename Value>
static ErrorOr<Value> value_or_error(Optional<Value> value)
{
    if (!value.has_value())
        return Error::from_string_literal("The session store could not do that");
    return value.release_value();
}

ErrorOr<WebView::SessionWindowId> RemoteSession::window_opened() { return value_or_error(HubConnection::the().session_window_opened(m_is_private)); }
ErrorOr<void> RemoteSession::window_closing(WebView::SessionStore::WindowClosing closing) { return error_unless(HubConnection::the().session_window_closing(m_is_private, move(closing))); }
void RemoteSession::window_activated(WebView::SessionWindowId id) { HubConnection::the().async_session_window_activated(m_is_private, id); }
void RemoteSession::window_geometry_changed(WebView::SessionWindowId id, WebView::SessionWindowGeometry geometry) { HubConnection::the().async_session_window_geometry_changed(m_is_private, id, geometry); }
ErrorOr<WebView::SessionTabId> RemoteSession::tab_opened(WebView::SessionStore::TabOpened opened)
{
    // NB: A new tab has no URL yet, and that cannot go over IPC. The store treats it as about:blank anyway.
    if (opened.initial_url.scheme().is_empty())
        opened.initial_url = URL::about_blank();
    return value_or_error(HubConnection::the().session_tab_opened(m_is_private, move(opened)));
}
ErrorOr<void> RemoteSession::tab_closed(WebView::SessionStore::TabClosed closed) { return error_unless(HubConnection::the().session_tab_closed(m_is_private, move(closed))); }
void RemoteSession::tab_detached(WebView::SessionTabId id) { HubConnection::the().async_session_tab_detached(m_is_private, id); }
void RemoteSession::tab_moved(WebView::SessionStore::TabMoved moved) { HubConnection::the().async_session_tab_moved(m_is_private, move(moved)); }
void RemoteSession::tab_order_changed(WebView::SessionStore::TabOrderChanged changed) { HubConnection::the().async_session_tab_order_changed(m_is_private, move(changed)); }
ErrorOr<void> RemoteSession::tab_pinned_changed(WebView::SessionTabId id, bool pinned) { return error_unless(HubConnection::the().session_tab_pinned_changed(m_is_private, id, pinned)); }
void RemoteSession::active_tab_changed(WebView::SessionTabId id) { HubConnection::the().async_session_active_tab_changed(m_is_private, id); }
void RemoteSession::application_quitting() { HubConnection::the().async_session_application_quitting(m_is_private); }
void RemoteSession::application_quit_aborted() { HubConnection::the().async_session_application_quit_aborted(m_is_private); }
u32 RemoteSession::consecutive_unclean_exits() const { return HubConnection::the().session_consecutive_unclean_exits(m_is_private); }
bool RemoteSession::has_closed_units() const { return HubConnection::the().session_has_closed_units(m_is_private); }
ErrorOr<Optional<WebView::ClosedSessionUnit>> RemoteSession::take_most_recently_closed() { return HubConnection::the().session_take_most_recently_closed(m_is_private); }
Vector<WebView::RecentlyClosedSessionUnit> RemoteSession::recently_closed_units(size_t limit) const { return HubConnection::the().session_recently_closed_units(m_is_private, limit); }
ErrorOr<Optional<WebView::ClosedSessionUnit>> RemoteSession::take_closed_unit(WebView::SessionWindowId id) { return HubConnection::the().session_take_closed_unit(m_is_private, id); }
Vector<WebView::RestoredSessionWindow> RemoteSession::take_launch_plan() { return HubConnection::the().session_take_launch_plan(m_is_private); }
Optional<WebView::SessionHistorySnapshot> RemoteSession::load_tab_history(WebView::SessionTabId id) { return HubConnection::the().session_load_tab_history(m_is_private, id); }
Optional<WebView::HistoryEntry> RemoteSession::entry_for_url(URL::URL const& url) const { return HubConnection::the().history_entry_for_url(m_is_private, url); }
#else
static WebView::SessionStore& store_of(WebView::IsPrivate is_private)
{
    return *WebView::Application::session_for_new_view(is_private)->session_store;
}

ErrorOr<WebView::SessionWindowId> RemoteSession::window_opened() { return store_of(m_is_private).window_opened(); }
ErrorOr<void> RemoteSession::window_closing(WebView::SessionStore::WindowClosing closing) { return store_of(m_is_private).window_closing(move(closing)); }
void RemoteSession::window_activated(WebView::SessionWindowId id) { store_of(m_is_private).window_activated(id); }
void RemoteSession::window_geometry_changed(WebView::SessionWindowId id, WebView::SessionWindowGeometry geometry) { store_of(m_is_private).window_geometry_changed(id, geometry); }
ErrorOr<WebView::SessionTabId> RemoteSession::tab_opened(WebView::SessionStore::TabOpened opened) { return store_of(m_is_private).tab_opened(move(opened)); }
ErrorOr<void> RemoteSession::tab_closed(WebView::SessionStore::TabClosed closed) { return store_of(m_is_private).tab_closed(move(closed)); }
void RemoteSession::tab_detached(WebView::SessionTabId id) { store_of(m_is_private).tab_detached(id); }
void RemoteSession::tab_moved(WebView::SessionStore::TabMoved moved) { store_of(m_is_private).tab_moved(move(moved)); }
void RemoteSession::tab_order_changed(WebView::SessionStore::TabOrderChanged changed) { store_of(m_is_private).tab_order_changed(move(changed)); }
ErrorOr<void> RemoteSession::tab_pinned_changed(WebView::SessionTabId id, bool pinned) { return store_of(m_is_private).tab_pinned_changed(id, pinned); }
void RemoteSession::active_tab_changed(WebView::SessionTabId id) { store_of(m_is_private).active_tab_changed(id); }
void RemoteSession::application_quitting() { store_of(m_is_private).application_quitting(); }
void RemoteSession::application_quit_aborted() { store_of(m_is_private).application_quit_aborted(); }
u32 RemoteSession::consecutive_unclean_exits() const { return store_of(m_is_private).consecutive_unclean_exits(); }
bool RemoteSession::has_closed_units() const { return store_of(m_is_private).has_closed_units(); }
ErrorOr<Optional<WebView::ClosedSessionUnit>> RemoteSession::take_most_recently_closed() { return store_of(m_is_private).take_most_recently_closed(); }
Vector<WebView::RecentlyClosedSessionUnit> RemoteSession::recently_closed_units(size_t limit) const { return store_of(m_is_private).recently_closed_units(limit); }
ErrorOr<Optional<WebView::ClosedSessionUnit>> RemoteSession::take_closed_unit(WebView::SessionWindowId id) { return store_of(m_is_private).take_closed_unit(id); }
Vector<WebView::RestoredSessionWindow> RemoteSession::take_launch_plan() { return store_of(m_is_private).take_launch_plan(); }
Optional<WebView::SessionHistorySnapshot> RemoteSession::load_tab_history(WebView::SessionTabId id) { return store_of(m_is_private).load_tab_history(id); }
Optional<WebView::HistoryEntry> RemoteSession::entry_for_url(URL::URL const& url) const { return WebView::Application::session_for_new_view(m_is_private)->history_store->entry_for_url(url); }
#endif

}
