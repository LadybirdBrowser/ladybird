/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
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
WEBVIEW_API Optional<String> flatpak_runtime_from(StringView flatpak_info);
WEBVIEW_API Optional<String> package_target_from(StringView package_target);
WEBVIEW_API Optional<String> hypervisor_from_dmi(StringView sys_vendor, StringView product_name);
WEBVIEW_API StringView session_type_from(StringView xdg_session_type, StringView wayland_display, StringView x11_display);
WEBVIEW_API String desktop_from(StringView xdg_current_desktop);
WEBVIEW_API StringView container_kind_from(StringView container_variable);
WEBVIEW_API StringView version_number_from(StringView);
WEBVIEW_API String describe_memory_size(u64 bytes);
WEBVIEW_API StringView install_method_name(InstallMethod);
WEBVIEW_API SystemDescription describe_current_system();

// A line of a report about the system it came from, and the key of the field it is sent as.
struct ReportedSystemField {
    StringView name;
    StringView key;
};

inline constexpr Array reported_system_fields {
    ReportedSystemField { "Kernel release"sv, "kernel_release"sv },
    ReportedSystemField { "OS version"sv, "os_version"sv },
    ReportedSystemField { "Distribution"sv, "distribution"sv },
    ReportedSystemField { "Install method"sv, "install_method"sv },
    ReportedSystemField { "Package target"sv, "package_target"sv },
    ReportedSystemField { "Flatpak runtime"sv, "flatpak_runtime"sv },
    ReportedSystemField { "Container"sv, "container"sv },
    ReportedSystemField { "Virtual machine"sv, "virtual_machine"sv },
    ReportedSystemField { "Logical CPUs"sv, "logical_cpus"sv },
    ReportedSystemField { "Memory"sv, "memory"sv },
    ReportedSystemField { "Desktop"sv, "desktop"sv },
    ReportedSystemField { "Session type"sv, "session_type"sv },
    ReportedSystemField { "UI toolkit"sv, "ui_toolkit"sv },
    ReportedSystemField { "UI platform"sv, "ui_platform"sv },
};

struct ReportedSystemDetail {
    ReportedSystemField field;
    String value;
};

// What a report says of the system it came from: the kind of installation it is, and nothing that tells one
// installation apart from another of the same kind.
WEBVIEW_API Vector<ReportedSystemDetail> describe_system_for_report();
WEBVIEW_API void append_system_for_report(StringBuilder&, ReadonlySpan<ReportedSystemDetail>);

// Only the process that runs the UI toolkit can describe it.
WEBVIEW_API void set_ui_toolkit_for_reports(String toolkit, String platform);

}
