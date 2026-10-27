/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/NotificationBackend.h>

namespace WebView {

// The backend of a platform with no notification support: every show fails, so pages see an error event.
class NoOpNotificationBackend final : public NotificationBackend {
public:
    virtual void show(PlatformNotification const& notification) override
    {
        if (on_failed)
            on_failed(notification.id);
    }

    virtual void close(u64) override { }
};

}
