/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/WebView/BrowsingBehavior.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, WebView::BrowsingBehavior const& browsing_behavior)
{
    TRY(encoder.encode(browsing_behavior.enable_autoscroll));
    TRY(encoder.encode(browsing_behavior.enable_primary_paste));

    return {};
}

template<>
ErrorOr<WebView::BrowsingBehavior> decode(Decoder& decoder)
{
    auto enable_autoscroll = TRY(decoder.decode<bool>());
    auto enable_primary_paste = TRY(decoder.decode<bool>());

    return WebView::BrowsingBehavior { enable_autoscroll, enable_primary_paste };
}

}
