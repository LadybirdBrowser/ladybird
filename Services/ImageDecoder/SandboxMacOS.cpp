/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ImageDecoder/Sandbox.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>

namespace ImageDecoder {

ErrorOr<void> apply_sandbox(StringView mach_server_name)
{
    TRY(Sandbox::configure_runtime());

    // Images are decoded into IOSurfaces, which the GPU samples where they are. CoreFoundation looks up the main bundle
    // when IOSurface sets itself up, so the decoder reads its own executable and application bundle.
    auto executable_path = TRY(Core::System::current_executable_path());

    Vector<Sandbox::SeatbeltPath> paths;
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, executable_path, Sandbox::SeatbeltPath::Access::ReadOnly));
    if (auto bundle = Sandbox::application_bundle_for_executable(executable_path); bundle.has_value())
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, *bundle, Sandbox::SeatbeltPath::Access::ReadOnly));

    return Sandbox::apply_macos_sandbox({
        .paths = paths.span(),
        .mach_server_name = mach_server_name,
        .system_services = Sandbox::SystemService::IOSurface,
    });
}

}
