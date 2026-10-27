/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/NotificationAttribution.h>
#include <LibWebView/NotificationManager.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

NotificationManager& NotificationManager::the()
{
    static auto& manager = *new NotificationManager;
    return manager;
}

NotificationBackend& NotificationManager::backend()
{
    if (!m_backend)
        set_backend(Application::the().create_notification_backend());
    return *m_backend;
}

void NotificationManager::set_backend(NonnullOwnPtr<NotificationBackend> backend)
{
    m_backend = move(backend);
    install_backend_callbacks();
}

void NotificationManager::install_backend_callbacks()
{
    m_backend->on_shown = [this](u64 id) { backend_did_show(id); };
    m_backend->on_failed = [this](u64 id) { backend_did_fail(id); };
    m_backend->on_clicked = [this](u64 id) { backend_did_click(id); };
    m_backend->on_closed = [this](u64 id, NotificationBackend::ByUser by_user) { backend_did_close(id, by_user); };
}

void NotificationManager::show(WebContentPage& page, u64 page_notification_id, Web::NotificationsAPI::NotificationData data, RefPtr<Gfx::Bitmap const> icon)
{
    // The notification belongs to the origin the browser knows the page's top-level document by, never to one the
    // page reports. (The document's origin, not the URL's: An opaque origin, which file: pages have, is the same
    // one for the life of the document, where a URL would make a fresh one on every ask.)
    auto url = page.view().url();
    auto origin = page.traversable().active_document().origin();

    Record record {
        .id = m_next_id++,
        .page = page,
        .page_id = page.id(),
        .page_notification_id = page_notification_id,
        .origin = origin,
        .origin_url = URL::Parser::basic_parse(origin.serialize()).value_or(url),
        .traversable = page.traversable().make_weak_ptr<CanonicalTraversable>(),
        .tag = data.tag,
    };

    // Same origin and same tag take the place of the earlier notification (the spec's oldNotification): Its page
    // hears that it closed, and the platform replaces it in place.
    Optional<u64> replaces;
    if (!data.tag.is_empty()) {
        for (size_t index = 0; index < m_records.size(); ++index) {
            auto const& existing = m_records[index];
            if (existing.tag == data.tag && existing.origin.is_same_origin(origin)) {
                replaces = existing.id;
                report_closed(existing, false);
                m_records.remove(index);
                break;
            }
        }
    }

    // An origin shows a bounded number of notifications at a time; the oldest makes room for a new one.
    size_t live_for_origin = 0;
    for (auto const& existing : m_records) {
        if (existing.origin.is_same_origin(origin))
            ++live_for_origin;
    }
    while (live_for_origin >= max_live_notifications_per_origin) {
        for (size_t index = 0; index < m_records.size(); ++index) {
            if (m_records[index].origin.is_same_origin(origin)) {
                close_record_from_browser(index);
                --live_for_origin;
                break;
            }
        }
    }

    auto id = record.id;
    m_records.append(move(record));
    backend().show(PlatformNotification {
        .id = id,
        .page_id = page.id(),
        .replaces = replaces,
        .data = move(data),
        .attribution = attribution_for_origin(origin),
        .icon = move(icon),
    });
}

void NotificationManager::close(WebContentPage& page, u64 page_notification_id)
{
    if (auto index = index_of(page, page_notification_id); index.has_value())
        backend().close(m_records[*index].id);
}

void NotificationManager::abandon(WebContentPage& page, u64 page_notification_id)
{
    if (auto index = index_of(page, page_notification_id); index.has_value())
        m_records[*index].page = nullptr;
}

void NotificationManager::click_handled(WebContentPage& page, u64 page_notification_id, bool into_focus)
{
    // Only a click the browser sent can bring the tab into focus; the notification itself may be gone by now, since
    // a click takes it down.
    auto pending = m_clicks_awaiting_reply.find_first_index_if([&](auto const& reply) {
        return reply.page == &page && reply.page_notification_id == page_notification_id;
    });
    if (!pending.has_value())
        return;
    m_clicks_awaiting_reply.remove(*pending);
    if (into_focus)
        Application::the().focus_page_from_notification(page);
}

void NotificationManager::page_did_close(WebContentPage& page)
{
    // The notifications stay up; only their way back to the page is gone.
    for (auto& record : m_records) {
        if (record.page == &page)
            record.page = nullptr;
    }
}

Optional<u64> NotificationManager::id_for(WebContentPage const& page, u64 page_notification_id) const
{
    if (auto index = index_of(page, page_notification_id); index.has_value())
        return m_records[*index].id;
    return {};
}

Optional<size_t> NotificationManager::index_of(u64 id) const
{
    for (size_t index = 0; index < m_records.size(); ++index) {
        if (m_records[index].id == id)
            return index;
    }
    return {};
}

Optional<size_t> NotificationManager::index_of(WebContentPage const& page, u64 page_notification_id) const
{
    for (size_t index = 0; index < m_records.size(); ++index) {
        auto const& record = m_records[index];
        if (record.page == &page && record.page_notification_id == page_notification_id)
            return index;
    }
    return {};
}

// Withdraws a notification the browser itself decided to drop, and tells its page.
void NotificationManager::close_record_from_browser(size_t index)
{
    auto record = m_records.take(index);
    backend().close(record.id);
    report_closed(record, false);
}

void NotificationManager::report_closed(Record const& record, bool by_user)
{
    if (record.page && record.page->is_open())
        record.page->async_notification_closed(record.page_notification_id, by_user);
}

void NotificationManager::backend_did_show(u64 id)
{
    auto index = index_of(id);
    if (!index.has_value())
        return;
    auto const& record = m_records[*index];
    if (record.page && record.page->is_open())
        record.page->async_notification_shown(record.page_notification_id);
}

void NotificationManager::backend_did_fail(u64 id)
{
    auto index = index_of(id);
    if (!index.has_value())
        return;
    auto record = m_records.take(*index);
    if (record.page && record.page->is_open())
        record.page->async_notification_show_failed(record.page_notification_id);
}

void NotificationManager::backend_did_click(u64 id)
{
    auto index = index_of(id);
    if (!index.has_value())
        return;
    auto const& record = m_records[*index];
    if (record.page && record.page->is_open()) {
        m_clicks_awaiting_reply.append({ record.page, record.page_notification_id });
        record.page->async_notification_clicked(record.page_notification_id);
        return;
    }

    // The page is gone, so the click opens the site instead; the notification has done its job.
    auto origin_url = record.origin_url;
    auto page_id = record.page_id;
    auto traversable = record.traversable;
    m_records.remove(*index);
    Application::the().open_url_in_new_tab_from_notification(origin_url, page_id, traversable.ptr());
}

void NotificationManager::backend_did_close(u64 id, NotificationBackend::ByUser by_user)
{
    auto index = index_of(id);
    if (!index.has_value())
        return;
    auto record = m_records.take(*index);
    report_closed(record, by_user == NotificationBackend::ByUser::Yes);
}

}
