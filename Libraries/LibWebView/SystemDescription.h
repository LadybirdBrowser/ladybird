/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibWebView/Export.h>

namespace WebView {

enum class InstallMethod : u8 {
    MacOSApp,
    DEB,
    RPM,
    Flatpak,
    AppImage,
    Source,
};

enum class Architecture : u8 {
    X86_64,
    AArch64,
    Other,
};

struct OSRelease {
    Optional<String> id;
    Optional<String> version_id;
};

struct SystemDescription {
    InstallMethod install_method { InstallMethod::Source };
    Architecture architecture { Architecture::Other };
    OSRelease os_release;
    Optional<String> macos_version;
};

WEBVIEW_API OSRelease parse_os_release(StringView);
WEBVIEW_API Optional<InstallMethod> parse_package_format(StringView);
WEBVIEW_API SystemDescription describe_current_system();

}
