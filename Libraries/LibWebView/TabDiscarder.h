/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/kmalloc.h>
#include <LibCore/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

// Discards the tabs that the user doesn't see, and wouldn't miss for now, when the system is about to run out of
// memory.
class WEBVIEW_API TabDiscarder {
    AK_MAKE_NONCOPYABLE(TabDiscarder);
    AK_MAKE_NONMOVABLE(TabDiscarder);

public:
    AK_ALLOC_WITH_KMALLOC;

    TabDiscarder();
    ~TabDiscarder();

private:
    void check_tabs();

    NonnullRefPtr<Core::Timer> m_timer;
};

}
