/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibURL/Origin.h>
#include <LibWebView/Export.h>

namespace WebView {

// The origin as a notification shows it to the user: the host and any explicit port, without the scheme for http(s),
// so that "example.com" reads as the site. An opaque origin reads as "this page".
WEBVIEW_API String attribution_for_origin(URL::Origin const&);

// A page's string as a platform takes it: valid UTF-8, with every lone surrogate replaced. A platform handed a lone
// surrogate's bytes may refuse the string, or worse.
WEBVIEW_API String platform_text(Utf16String const&);

}
