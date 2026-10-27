/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/kmalloc.h>
#include <LibGfx/Bitmap.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/NotificationsAPI/NotificationData.h>
#include <LibWebView/Forward.h>
#include <LibWebView/NotificationBackend.h>

namespace WebView {

// Owns the notifications the browser has shown, each with the page that asked for it and the origin it belongs to,
// and drives the platform backend. A notification outlives its page: a click on one whose page is gone opens the
// origin in a new tab. Same origin and same tag replace an earlier notification, and an origin keeps at most
// max_live_notifications_per_origin at a time.
class WEBVIEW_API NotificationManager {
public:
    AK_ALLOC_WITH_KMALLOC;

    static NotificationManager& the();

    static constexpr size_t max_live_notifications_per_origin = 8;

    void show(WebContentPage&, u64 page_notification_id, Web::NotificationsAPI::NotificationData, RefPtr<Gfx::Bitmap const> icon);
    void close(WebContentPage&, u64 page_notification_id);
    void click_handled(WebContentPage&, u64 page_notification_id, bool into_focus);
    void page_did_close(WebContentPage&);

    NotificationBackend& backend();
    void set_backend(NonnullOwnPtr<NotificationBackend>);

    // The manager's id of a page's notification, for tests that act on it through the backend.
    Optional<u64> id_for(WebContentPage const&, u64 page_notification_id) const;

private:
    struct Record {
        u64 id { 0 };
        // Null once the page has closed; the record then only serves a later click.
        RefPtr<WebContentPage> page;
        u64 page_notification_id { 0 };
        URL::Origin origin;
        // What a click opens once the page is gone.
        URL::URL origin_url;
        WeakPtr<CanonicalTraversable> traversable;
        Utf16String tag;
    };

    NotificationManager() = default;

    void install_backend_callbacks();
    Optional<size_t> index_of(u64 id) const;
    Optional<size_t> index_of(WebContentPage const&, u64 page_notification_id) const;
    void close_record_from_browser(size_t index);
    void report_closed(Record const&, bool by_user);

    void backend_did_show(u64 id);
    void backend_did_fail(u64 id);
    void backend_did_click(u64 id);
    void backend_did_close(u64 id, NotificationBackend::ByUser);

    OwnPtr<NotificationBackend> m_backend;
    Vector<Record> m_records;
    u64 m_next_id { 1 };
};

}
