/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace WebView {

struct BrowsingBehavior {
    bool enable_autoscroll { true };
    bool enable_primary_paste { true };
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::BrowsingBehavior const&);

template<>
WEBCOMMON_API ErrorOr<WebView::BrowsingBehavior> decode(Decoder&);

}
