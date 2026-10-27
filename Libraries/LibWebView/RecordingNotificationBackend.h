/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/NotificationBackend.h>

namespace WebView {

// A backend that keeps the notifications it was asked to show, for tests to read and to act on as the user would.
class WEBVIEW_API RecordingNotificationBackend final : public NotificationBackend {
public:
    virtual void show(PlatformNotification const&) override;
    virtual void close(u64 id) override;

    // What the user would do with a shown notification.
    void click(u64 id);
    void dismiss(u64 id);

    virtual RecordingNotificationBackend* as_recording_backend() override { return this; }

    Vector<PlatformNotification> const& shown() const { return m_shown; }

    // What the browser did on a click: the pages it brought into focus, and the URLs it opened for notifications
    // whose page was gone. The test application records them here.
    void record_focus(Web::PageId);
    void record_open(URL::URL const&, Web::PageId originating_page);

    // Everything above, for the notifications of one page, as JSON for a test page to read. Tests run side by side in
    // one UI process, so each sees only its own.
    String state_json(Web::PageId) const;
    // Forgets a page's history, since test-web hands the page id to the next test it runs in the same view.
    void forget_page(Web::PageId);

    // When set, the next show() fails instead, as a platform that refuses the notification would.
    bool fail_next_show { false };

private:
    Optional<size_t> index_of(u64 id) const;

    struct Closed {
        u64 id { 0 };
        Web::PageId page_id;
    };
    struct Opened {
        URL::URL url;
        Web::PageId page_id;
    };

    Vector<PlatformNotification> m_shown;
    Vector<Closed> m_closed;
    Vector<Web::PageId> m_focused;
    Vector<Opened> m_opened;
    HashMap<u64, Web::PageId> m_page_of;
};

}
