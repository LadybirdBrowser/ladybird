/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>
#include <LibWebView/NotificationBackend.h>

namespace WebView {

// What a Desktop Notifications daemon (org.freedesktop.Notifications) says it can show, from its GetCapabilities.
struct FreedesktopCapabilities {
    bool body { false };
    bool body_markup { false };
    bool persistence { false };
    bool actions { false };
};

// The "image-data" hint: raw RGBA pixels, (iiibiiay) on the wire.
struct FreedesktopImageData {
    i32 width { 0 };
    i32 height { 0 };
    i32 rowstride { 0 };
    bool has_alpha { true };
    i32 bits_per_sample { 8 };
    i32 channels { 4 };
    ByteBuffer pixels;
};

struct FreedesktopHint {
    String name;
    Variant<u8, bool, String, FreedesktopImageData> value;
};

// The arguments of a Notify call (app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout).
struct FreedesktopNotifyArguments {
    String app_name;
    u32 replaces_id { 0 };
    String app_icon;
    String summary;
    String body;
    Vector<String> actions;
    Vector<FreedesktopHint> hints;
    i32 expire_timeout { 0 };
};

// Lays a notification out for the daemon: the attribution opens the body, markup is escaped only for a daemon that
// renders it, a notification that requires interaction never expires and is critical, and one shown by a daemon
// without a notification center (no "persistence") expires after 25 seconds rather than at the daemon's whim.
WEBVIEW_API FreedesktopNotifyArguments build_notify_arguments(PlatformNotification const&, FreedesktopCapabilities const&, u32 replaces_daemon_id);

}
