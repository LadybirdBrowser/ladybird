/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibWebView/Export.h>

namespace WebView {

// Shows an indicator while what it indicates is active, and for a linger once that stops, so the user sees it happened
// even when it stops at once. Only stopping starts the linger: being told again that it's inactive doesn't extend it,
// and becoming active again cancels it.
class WEBVIEW_API LingeringIndicator {
public:
    explicit LingeringIndicator(AK::Duration linger)
        : m_linger(linger)
    {
    }

    void set_active(bool, MonotonicTime now);

    bool is_active() const { return m_active; }
    bool is_visible(MonotonicTime now) const;
    Optional<MonotonicTime> visible_until() const { return m_visible_until; }

private:
    AK::Duration m_linger;
    bool m_active { false };
    Optional<MonotonicTime> m_visible_until;
};

}
