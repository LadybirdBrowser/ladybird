/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Bindings/Notification.h>
#include <LibWebCommon/Export.h>

namespace Web::NotificationsAPI {

// What the UI process needs to show a notification: the page-supplied fields of a notification (see the "notification"
// concept in https://notifications.spec.whatwg.org/#notifications). The origin isn't among them, since the UI
// process attributes a notification to the origin it knows the page by, never to one the page reports.
struct NotificationData {
    Utf16String title;
    Utf16String body;
    Utf16String tag;
    Utf16String language;
    Bindings::NotificationDirection direction { Bindings::NotificationDirection::Auto };
    Optional<bool> silent;
    bool require_interaction { false };
    bool renotify { false };
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::NotificationsAPI::NotificationData const&);

template<>
WEBCOMMON_API ErrorOr<Web::NotificationsAPI::NotificationData> decode(Decoder&);

}
