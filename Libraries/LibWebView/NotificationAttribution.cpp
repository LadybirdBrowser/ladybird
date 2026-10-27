/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16View.h>
#include <LibWebView/NotificationAttribution.h>

namespace WebView {

String platform_text(Utf16String const& text)
{
    StringBuilder builder;
    for (auto code_point : text.utf16_view()) {
        if (is_unicode_surrogate(code_point))
            builder.append_code_point(0xFFFD);
        else
            builder.append_code_point(code_point);
    }
    return MUST(builder.to_string());
}

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
