/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/Export.h>
#include <LibWebView/Profile.h>

namespace WebView {

struct SessionTab {
    URL::URL url;
};

struct SessionWindow {
    Vector<SessionTab> tabs;
    size_t active_tab_index { 0 };
    Optional<i32> x;
    Optional<i32> y;
    Optional<i32> width;
    Optional<i32> height;
    bool maximized { false };
};

struct Session {
    Vector<SessionWindow> windows;
    size_t active_window_index { 0 };

    bool is_empty() const
    {
        for (auto const& window : windows) {
            if (!window.tabs.is_empty())
                return false;
        }
        return true;
    }
};

WEBVIEW_API ByteString session_store_path(Profile const&);
WEBVIEW_API ErrorOr<Optional<Session>> load_session(Profile const&);
WEBVIEW_API ErrorOr<void> save_session(Profile const&, Session const&);
WEBVIEW_API ErrorOr<void> clear_session(Profile const&);

}
