/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/Sandbox.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>

namespace AudioServer {

ErrorOr<void> apply_sandbox(StringView mach_server_name)
{
    TRY(Sandbox::configure_runtime());

    auto executable_path = TRY(Core::System::current_executable_path());
    Vector<Sandbox::SeatbeltPath> paths;
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, executable_path, Sandbox::SeatbeltPath::Access::ReadOnly));
    // The audio frameworks look up the main bundle of the process.
    if (auto bundle = Sandbox::application_bundle_for_executable(executable_path); bundle.has_value())
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, *bundle, Sandbox::SeatbeltPath::Access::ReadOnly));

    return Sandbox::apply_macos_sandbox({
        .paths = paths.span(),
        .mach_server_name = mach_server_name,
        .system_services = Sandbox::SystemService::Audio,
    });
}

}
