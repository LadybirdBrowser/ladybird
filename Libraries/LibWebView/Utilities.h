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

// Opens a local file a renderer asked for, read-only, once its symbolic links are resolved. Anything but a regular file
// or a directory is refused, as Core::File::open_regular_file_or_directory() refuses it, and so is a file on devfs on
// macOS, or a process information or sysfs file on Linux.
WEBVIEW_API ErrorOr<IPC::File> open_local_file_for_renderer(ByteString const& path);

WEBVIEW_API ErrorOr<JsonObject> read_json_file(ByteString const& path);
WEBVIEW_API ErrorOr<void> write_json_file(ByteString const& path, JsonValue const& value);

}
