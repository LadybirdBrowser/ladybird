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
#include <LibIPC/File.h>
#include <LibWebCommon/Forward.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <LibWebView/Forward.h>

namespace WebView {

WEBVIEW_API ErrorOr<Web::HTML::SelectedFile> create_selected_file(ByteString const&);

// Opens a local file a renderer asked for, read-only. Only regular files and directories are opened, and the open
// never blocks, so a path naming a FIFO or a device cannot stall the UI process.
WEBVIEW_API ErrorOr<IPC::File> open_local_file_for_renderer(ByteString const& path);

ErrorOr<JsonObject> read_json_file(ByteString const& path);
ErrorOr<void> write_json_file(ByteString const& path, JsonValue const& value);

}
