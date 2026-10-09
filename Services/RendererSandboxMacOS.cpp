/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <LibWebView/Utilities.h>
#include <Services/RendererSandbox.h>

namespace RendererSandbox {

ErrorOr<void> apply_sandbox(StringView mach_server_name)
{
    TRY(Sandbox::configure_runtime());

    auto executable_path = TRY(Core::System::current_executable_path());

    Vector<Sandbox::SeatbeltPath> paths;
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, WebView::s_ladybird_resource_root, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, executable_path, Sandbox::SeatbeltPath::Access::ReadOnly));

    // The helpers read their own application bundle, for example when CoreFoundation looks up the main bundle.
    if (auto bundle = Sandbox::application_bundle_for_executable(executable_path); bundle.has_value())
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, *bundle, Sandbox::SeatbeltPath::Access::ReadOnly));

    // Every renderer draws and runs WebAssembly. Media decodes in the MediaServer and audio plays in the AudioServer.
    auto system_services = Sandbox::SystemService::Fonts | Sandbox::SystemService::IOSurface | Sandbox::SystemService::JIT;

    return Sandbox::apply_macos_sandbox({
        .paths = paths.span(),
        .mach_server_name = mach_server_name,
        .system_services = system_services,
    });
}

}
