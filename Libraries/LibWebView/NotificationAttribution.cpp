/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/NotificationAttribution.h>

namespace WebView {

String attribution_for_origin(URL::Origin const& origin)
{
    if (origin.is_opaque())
        return "this page"_string;

    auto serialized = origin.serialize();
    for (auto prefix : { "https://"sv, "http://"sv }) {
        if (serialized.starts_with_bytes(prefix))
            return MUST(serialized.substring_from_byte_offset(prefix.length()));
    }
    return serialized;
}

}
