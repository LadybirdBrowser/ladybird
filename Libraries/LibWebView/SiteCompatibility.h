/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonValue.h>
#include <AK/StringView.h>
#include <LibWebView/Export.h>

namespace WebView {

WEBVIEW_API ErrorOr<JsonValue> load_site_compatibility_data(StringView directory_uri = "resource://ladybird/site-compatibility"sv);

}
