/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <RequestServer/ResourceSubstitutionMap.h>
#include <RequestServer/Sandbox.h>
#include <openssl/x509.h>
#include <string.h>

namespace RequestServer {

ErrorOr<void> apply_sandbox(StringView mach_server_name, Vector<ByteString> const& certificates, StringView cache_path)
{
    TRY(Sandbox::configure_runtime());

    Vector<Sandbox::SeatbeltPath> paths;
    TRY(Core::Directory::create(cache_path, Core::Directory::CreateDirectories::Yes));

    auto executable_path = TRY(Core::System::current_executable_path());

    TRY(Sandbox::add_seatbelt_path_if_exists(paths, executable_path, Sandbox::SeatbeltPath::Access::ReadOnly));

    // The helpers read their own application bundle, for example when CoreFoundation looks up the main bundle.
    if (auto bundle = Sandbox::application_bundle_for_executable(executable_path); bundle.has_value())
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, *bundle, Sandbox::SeatbeltPath::Access::ReadOnly));

    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/etc/hosts"sv, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/etc/resolv.conf"sv, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/private/etc/hosts"sv, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/private/etc/resolv.conf"sv, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/private/etc/ssl"sv, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, "/Library/Preferences/com.apple.networkd.plist"sv, Sandbox::SeatbeltPath::Access::ReadOnly));

    // OpenSSL reads its trusted roots from these instead of /etc/ssl when they are set.
    auto const* certificate_file_variable = X509_get_default_cert_file_env();
    if (auto file = Core::Environment::get({ certificate_file_variable, strlen(certificate_file_variable) }); file.has_value() && !file->is_empty())
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, *file, Sandbox::SeatbeltPath::Access::ReadOnly));

    // The directory variable holds a colon-separated list of directories.
    auto const* certificate_directory_variable = X509_get_default_cert_dir_env();
    if (auto directories = Core::Environment::get({ certificate_directory_variable, strlen(certificate_directory_variable) }); directories.has_value()) {
        for (auto directory : directories->split_view(':'))
            TRY(Sandbox::add_seatbelt_path_if_exists(paths, directory, Sandbox::SeatbeltPath::Access::ReadOnly));
    }

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

    return Sandbox::apply_macos_sandbox({
        .paths = paths.span(),
        .network_access = Sandbox::NetworkAccess::Allowed,
        .mach_server_name = mach_server_name,
    });
}

}
