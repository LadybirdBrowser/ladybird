/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <WebRTCClient/Sandbox.h>

namespace WebRTCClient {

ErrorOr<void> apply_sandbox(StringView mach_server_name, NetworkAccess network_access)
{
    TRY(Sandbox::configure_runtime());

    Vector<Sandbox::SeatbeltPath> paths;
    auto executable_path = TRY(Core::System::current_executable_path());
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, executable_path, Sandbox::SeatbeltPath::Access::ReadOnly));

    // The helpers read their own application bundle, for example when CoreFoundation looks up the main bundle.
    if (auto bundle = Sandbox::application_bundle_for_executable(executable_path); bundle.has_value())
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, *bundle, Sandbox::SeatbeltPath::Access::ReadOnly));

    return Sandbox::apply_macos_sandbox({
        .paths = paths.span(),
        .network_access = network_access == NetworkAccess::Allowed ? Sandbox::NetworkAccess::Datagrams : Sandbox::NetworkAccess::Denied,
        .mach_server_name = mach_server_name,
    });
}

}
