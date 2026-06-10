/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <RequestServer/ResourceSubstitutionMap.h>
#include <RequestServer/Sandbox.h>

namespace RequestServer {

ErrorOr<void> apply_sandbox(Vector<ByteString> const& certificates)
{
    TRY(Sandbox::configure_runtime());

    Vector<Sandbox::SeatbeltPath> paths;
    auto cache_path = TRY(String::formatted("{}/Ladybird", Core::StandardPaths::cache_directory()));
    TRY(Core::Directory::create(cache_path.to_byte_string(), Core::Directory::CreateDirectories::Yes));

    auto executable_path = TRY(Core::System::current_executable_path());
    auto build_root = LexicalPath::dirname(LexicalPath::dirname(LexicalPath::dirname(LexicalPath::dirname(LexicalPath::dirname(executable_path)))));

    TRY(Sandbox::add_seatbelt_path_if_exists(paths, executable_path, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, LexicalPath::join(build_root, "bin"sv).string(), Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, LexicalPath::join(build_root, "lib"sv).string(), Sandbox::SeatbeltPath::Access::ReadAndExecute));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, LexicalPath::join(build_root, "vcpkg_installed"sv).string(), Sandbox::SeatbeltPath::Access::ReadAndExecute));

    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/etc/hosts"sv, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/etc/resolv.conf"sv, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/private/etc/hosts"sv, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/private/etc/resolv.conf"sv, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/private/etc/ssl"sv, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/Library/Preferences/com.apple.networkd.plist"sv, Sandbox::SeatbeltPath::Access::ReadOnly));

    for (auto const& certificate : certificates) {
        auto certificate_path = LexicalPath::dirname(certificate);
        if (certificate_path.is_empty())
            certificate_path = ".";

        TRY(Sandbox::add_seatbelt_path_if_exists(paths, certificate_path, Sandbox::SeatbeltPath::Access::ReadOnly));
    }

    if (g_resource_substitution_map) {
        TRY(g_resource_substitution_map->for_each_substitution([&](auto const& substitution) -> ErrorOr<void> {
            TRY(Sandbox::add_seatbelt_path_if_exists(paths, substitution.file_path, Sandbox::SeatbeltPath::Access::ReadOnly));
            return {};
        }));
    }

    TRY(Sandbox::add_seatbelt_path_if_exists(paths, cache_path, Sandbox::SeatbeltPath::Access::ReadWrite));

    return Sandbox::apply_macos_sandbox(paths.span(), Sandbox::NetworkAccess::Allowed);
}

}
