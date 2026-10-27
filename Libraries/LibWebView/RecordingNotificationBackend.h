/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
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

    Vector<PlatformNotification> const& shown() const { return m_shown; }
    Vector<u64> const& closed() const { return m_closed; }

    // When set, the next show() fails instead, as a platform that refuses the notification would.
    bool fail_next_show { false };

private:
    Optional<size_t> index_of(u64 id) const;

    Vector<PlatformNotification> m_shown;
    Vector<u64> m_closed;
};

}
