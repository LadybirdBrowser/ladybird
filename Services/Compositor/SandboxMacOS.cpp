/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/LexicalPath.h>
#include <Compositor/Sandbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <LibCore/Directory.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <LibWebView/Utilities.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

namespace Compositor {

static ErrorOr<Optional<ByteString>> application_darwin_user_cache_directory()
{
    char darwin_user_cache_directory[PATH_MAX];
    if (confstr(_CS_DARWIN_USER_CACHE_DIR, darwin_user_cache_directory, sizeof(darwin_user_cache_directory)) == 0)
        return OptionalNone {};

    auto bundle_identifier = CFBundleGetIdentifier(CFBundleGetMainBundle());
    if (!bundle_identifier)
        return OptionalNone {};

    char bundle_identifier_buffer[256];
    if (!CFStringGetCString(bundle_identifier, bundle_identifier_buffer, sizeof(bundle_identifier_buffer), kCFStringEncodingUTF8))
        return OptionalNone {};

    return LexicalPath::join(StringView { darwin_user_cache_directory, strlen(darwin_user_cache_directory) }, StringView { bundle_identifier_buffer, strlen(bundle_identifier_buffer) }).string();
}

ErrorOr<void> apply_sandbox(StringView mach_server_name, StringView cache_path)
{
    TRY(Sandbox::configure_runtime());

    auto executable_path = TRY(Core::System::current_executable_path());
    auto build_root = LexicalPath::dirname(LexicalPath::dirname(LexicalPath::dirname(LexicalPath::dirname(LexicalPath::dirname(executable_path)))));

    Vector<Sandbox::SeatbeltPath> paths;
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, executable_path, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, LexicalPath::join(build_root, "bin"sv).string(), Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, LexicalPath::join(build_root, "lib"sv).string(), Sandbox::SeatbeltPath::Access::ReadAndExecute));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, LexicalPath::join(build_root, "vcpkg_installed"sv).string(), Sandbox::SeatbeltPath::Access::ReadAndExecute));

    TRY(Sandbox::add_seatbelt_path_if_exists(paths, TRY(String::formatted("{}/fonts", WebView::s_ladybird_resource_root)), Sandbox::SeatbeltPath::Access::ReadOnly));

    TRY(Core::Directory::create(cache_path, Core::Directory::CreateDirectories::Yes));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, cache_path, Sandbox::SeatbeltPath::Access::ReadWrite));

    // Metal keeps its shader caches in the Darwin user cache directory, in a directory named after the application's
    // bundle identifier. The rest of that directory belongs to other applications.
    if (auto metal_cache_directory = TRY(application_darwin_user_cache_directory()); metal_cache_directory.has_value()) {
        TRY(Core::Directory::create(*metal_cache_directory, Core::Directory::CreateDirectories::Yes));
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, *metal_cache_directory, Sandbox::SeatbeltPath::Access::ReadWrite));
    }

    // ANGLE's Metal backend opens one of these while creating WebGL contexts, depending on whether the GPU is real
    // hardware or the paravirtualized device of a virtual machine.
    static constexpr Array metal_iokit_user_client_classes {
        "AGXDeviceUserClient"sv,
        "AppleParavirtDeviceUserClient"sv,
    };

    return Sandbox::apply_macos_sandbox({
        .paths = paths.span(),
        .iokit_user_client_classes = metal_iokit_user_client_classes,
        .mach_server_name = mach_server_name,
        .system_services = Sandbox::SystemService::Fonts | Sandbox::SystemService::GPU | Sandbox::SystemService::IOSurface,
    });
}

}
