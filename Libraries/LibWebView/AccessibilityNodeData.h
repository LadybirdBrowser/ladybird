/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API AccessibilityNodeData {
    i64 id { 0 };
    i64 parent_id { -1 };
    Vector<i64> child_ids;

    String role;
    String name;
    String description;
    String value;

    Gfx::IntRect bounds;

    bool is_focused { false };
    bool is_disabled { false };
    i32 heading_level { 0 };

    String live; // "assertive", "polite", or empty
};

}

namespace IPC {

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::AccessibilityNodeData const&);

template<>
WEBVIEW_API ErrorOr<WebView::AccessibilityNodeData> decode(Decoder&);

}
