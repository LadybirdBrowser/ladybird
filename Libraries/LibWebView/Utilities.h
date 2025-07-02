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
#include <AK/Vector.h>
#include <LibWebView/Forward.h>

namespace WebView {

WEBVIEW_API void platform_init(Optional<ByteString> ladybird_binary_path = {});
WEBVIEW_API void copy_default_config_files(StringView config_path);
WEBVIEW_API ErrorOr<Vector<ByteString>> get_paths_for_helper_process(StringView process_name);

WEBVIEW_API extern ByteString s_ladybird_resource_root;
WEBVIEW_API Optional<ByteString const&> mach_server_name();
WEBVIEW_API void set_mach_server_name(ByteString name);

WEBVIEW_API ErrorOr<void> handle_attached_debugger();

}
