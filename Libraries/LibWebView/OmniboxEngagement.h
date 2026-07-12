/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibWebView/Export.h>

namespace WebView {

enum class OmniboxDestinationKind : u8 {
    URL,
    Search,
};

struct WEBVIEW_API OmniboxEngagement {
    String input;
    OmniboxDestinationKind destination_kind { OmniboxDestinationKind::URL };
    String destination;
    bool was_explicit { false };
    UnixDateTime used_at { UnixDateTime::now() };
};

struct WEBVIEW_API StoredOmniboxEngagement {
    String normalized_input;
    OmniboxDestinationKind destination_kind { OmniboxDestinationKind::URL };
    String destination;
    u64 explicit_use_count { 0 };
    u64 default_use_count { 0 };
    UnixDateTime last_used_time;
};

WEBVIEW_API String normalize_omnibox_input(StringView, OmniboxDestinationKind);
WEBVIEW_API String normalize_omnibox_destination(StringView, OmniboxDestinationKind);

}
