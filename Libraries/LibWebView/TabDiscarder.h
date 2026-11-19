/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/kmalloc.h>
#include <LibCore/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

// Discards the tabs that the user doesn't see, and wouldn't miss for now, when the system is about to run out of
// memory, or once they were hidden for as long as the user's settings allow.
class WEBVIEW_API TabDiscarder {
    AK_MAKE_NONCOPYABLE(TabDiscarder);
    AK_MAKE_NONMOVABLE(TabDiscarder);

public:
    AK_ALLOC_WITH_KMALLOC;

    TabDiscarder();
    ~TabDiscarder();

    // NB: Only a hidden tab may be discarded, so the tabs are checked only while there is one.
    static void did_hide_a_tab();

    // How long a tab must stay hidden before it is discarded, if hidden tabs are discarded at all.
    static Optional<AK::Duration> minimum_time_hidden_after_inactivity();

private:
    void check_tabs();

    NonnullRefPtr<Core::Timer> m_timer;
};

}
