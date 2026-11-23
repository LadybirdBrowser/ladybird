/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <LibWebView/SystemDescription.h>

#if defined(AK_OS_MACOS)
#    include <sys/sysctl.h>
#endif

namespace WebView {

// https://www.freedesktop.org/software/systemd/man/latest/os-release.html
OSRelease parse_os_release(StringView contents)
{
    OSRelease result;
    for (auto line : contents.lines()) {
        line = line.trim_whitespace();
        if (line.is_empty() || line.starts_with('#'))
            continue;

        auto equals = line.find('=');
        if (!equals.has_value())
            continue;
        auto key = line.substring_view(0, *equals);
        auto value = line.substring_view(*equals + 1);
        if (value.length() >= 2 && (value[0] == '"' || value[0] == '\'') && value[value.length() - 1] == value[0])
            value = value.substring_view(1, value.length() - 2);

        auto string = String::from_utf8(value);
        if (string.is_error())
            continue;
        if (key == "ID"sv)
            result.id = string.release_value();
        else if (key == "VERSION_ID"sv)
            result.version_id = string.release_value();
    }
    return result;
}

Optional<InstallMethod> parse_package_format(StringView contents)
{
    auto format = contents.trim_whitespace();
    if (format == "DEB"sv)
        return InstallMethod::DEB;
    if (format == "RPM"sv)
        return InstallMethod::RPM;
    if (format == "AppImage"sv)
        return InstallMethod::AppImage;
    return {};
}

static InstallMethod current_install_method()
{
#if defined(AK_OS_MACOS)
    return InstallMethod::MacOSApp;
#else
    if (FileSystem::exists("/.flatpak-info"sv))
        return InstallMethod::Flatpak;

    // The Linux packages say which kind they are in a file of their own, which an archive of the same build lacks.
    auto marker = LexicalPath::join(s_ladybird_resource_root, "package-format"sv).string();
    if (auto file = Core::File::open(marker, Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            if (auto install_method = parse_package_format(contents.value()); install_method.has_value())
                return *install_method;
        }
    }
    return InstallMethod::Source;
#endif
}

static OSRelease current_os_release()
{
    for (auto path : { "/etc/os-release"sv, "/usr/lib/os-release"sv }) {
        auto file = Core::File::open(path, Core::File::OpenMode::Read);
        if (file.is_error())
            continue;
        auto contents = file.value()->read_until_eof();
        if (!contents.is_error())
            return parse_os_release(contents.value());
    }
    return {};
}

static Optional<String> current_macos_version()
{
#if defined(AK_OS_MACOS)
    char version[32] {};
    size_t size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) != 0)
        return {};
    auto string = String::from_utf8(StringView { version, strnlen(version, sizeof(version)) });
    if (string.is_error())
        return {};
    return string.release_value();
#else
    return {};
#endif
}

SystemDescription describe_current_system()
{
    SystemDescription system;
    system.install_method = current_install_method();
#if ARCH(X86_64)
    system.architecture = Architecture::X86_64;
#elif ARCH(AARCH64)
    system.architecture = Architecture::AArch64;
#endif
    if (system.install_method != InstallMethod::MacOSApp && system.install_method != InstallMethod::Flatpak)
        system.os_release = current_os_release();
    system.macos_version = current_macos_version();
    return system;
}

}
