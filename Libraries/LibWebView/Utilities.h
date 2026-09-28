/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibWebCommon/Forward.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <LibWebView/Forward.h>

namespace WebView {

WEBVIEW_API ErrorOr<Web::HTML::SelectedFile> create_selected_file(ByteString const&);

ErrorOr<JsonObject> read_json_file(ByteString const& path);
ErrorOr<void> write_json_file(ByteString const& path, JsonValue const& value);

}
