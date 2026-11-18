/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWebView/Export.h>

namespace WebView {

// Whether the system opens the links of other applications in this browser.
enum class DefaultBrowserStatus : u8 {
    // The system has not answered yet.
    Unknown,
    // The system cannot tell, or the browser cannot change it.
    Unavailable,
    NotDefault,
    Default,
};

// How asking the system to make this browser the default ended. A user who turns the system's question down is not a
// failure.
struct MakeDefaultBrowserResult {
    bool failed { false };
    // What the system said of the failure, in its own words, if it said anything.
    Optional<String> system_message;
};

// The last status read from the system, and the way to change it.
class WEBVIEW_API DefaultBrowser {
    AK_ALLOC_WITH_KMALLOC;

public:
    virtual ~DefaultBrowser() = default;

    DefaultBrowserStatus status() const { return m_status; }

    void did_read_status(DefaultBrowserStatus status)
    {
        if (m_status == status)
            return;
        m_status = status;
        if (on_status_change)
            on_status_change();
    }

    virtual void refresh() = 0;

    // The system may ask the user first. on_complete runs on the main thread once it has answered, possibly before
    // this returns.
    virtual void make_default(Function<void(MakeDefaultBrowserResult const&)> on_complete) = 0;

    Function<void()> on_status_change;

private:
    DefaultBrowserStatus m_status { DefaultBrowserStatus::Unknown };
};

}
