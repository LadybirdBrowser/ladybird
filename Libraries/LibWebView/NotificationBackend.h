/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>
#include <LibGfx/Bitmap.h>
#include <LibWebCommon/NotificationsAPI/NotificationData.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// A notification as a platform backend shows it: the page's fields, plus what the browser adds to them.
struct PlatformNotification {
    // Names the notification for the life of the process; the backend reports every event by it.
    u64 id { 0 };
    // The page that asked for it, for a backend that keeps per-page state.
    Web::PageId page_id;
    // The notification with the same origin and tag that this one takes the place of, if any.
    Optional<u64> replaces;
    Web::NotificationsAPI::NotificationData data;
    // The origin the notification is shown as coming from. See attribution_for_origin().
    String attribution;
    RefPtr<Gfx::Bitmap const> icon;
};

// What a notification platform does for the browser. show() ends in on_shown() or on_failed() for its id, possibly
// before it returns; close() ends in on_closed() with ByUser::No. The platform reports the user's clicks and
// dismissals through on_clicked() and on_closed(); a click takes the notification down too, so it's followed by
// on_closed() with ByUser::Yes. Every callback runs on the main thread.
class WEBVIEW_API NotificationBackend {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~NotificationBackend() = default;

    enum class ByUser {
        No,
        Yes,
    };

    virtual void show(PlatformNotification const&) = 0;
    virtual void close(u64 id) = 0;

    // Non-null for the backend tests install, which keeps what it was shown.
    virtual RecordingNotificationBackend* as_recording_backend() { return nullptr; }

    Function<void(u64 id)> on_shown;
    Function<void(u64 id)> on_failed;
    Function<void(u64 id)> on_clicked;
    Function<void(u64 id, ByUser)> on_closed;
};

}
