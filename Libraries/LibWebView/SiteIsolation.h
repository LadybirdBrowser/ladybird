/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibWebView/Forward.h>

namespace WebView {

enum class SiteIsolationMode {
    Disabled,
    TopLevel,
    IFrame,
};

[[nodiscard]] WEBVIEW_API Optional<SiteIsolationMode> site_isolation_mode_from_string(StringView);
[[nodiscard]] WEBVIEW_API StringView site_isolation_mode_to_string(SiteIsolationMode);
[[nodiscard]] WEBVIEW_API SiteIsolationMode site_isolation_mode();
WEBVIEW_API void set_site_isolation_mode(SiteIsolationMode);

}
