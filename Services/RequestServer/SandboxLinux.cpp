/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <RequestServer/Sandbox.h>

namespace RequestServer {

ErrorOr<void> apply_sandbox(Vector<ByteString> const& certificates)
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    Vector<Sandbox::LandlockPath> paths;
    auto cache_path = TRY(String::formatted("{}/Ladybird", Core::StandardPaths::cache_directory()));
    TRY(Core::Directory::create(cache_path.to_byte_string(), Core::Directory::CreateDirectories::Yes));

    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/ssl"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/host.conf"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/hosts"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/nsswitch.conf"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/resolv.conf"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/run/systemd/resolve"sv, Sandbox::LandlockPath::Access::ReadOnly));

    for (auto const& certificate : certificates) {
        auto certificate_path = LexicalPath::dirname(certificate);
        if (certificate_path.is_empty())
            certificate_path = ".";

        TRY(Sandbox::add_landlock_path_if_exists(paths, certificate_path, Sandbox::LandlockPath::Access::ReadOnly));
    }

    TRY(Sandbox::add_landlock_path_if_exists(paths, cache_path, Sandbox::LandlockPath::Access::ReadWrite));

    TRY(Sandbox::restrict_filesystem_with_landlock(paths.span()));

    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_filesystem_writes();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_network();
    policy.allow_common_runtime();
    TRY(policy.install());

    return {};
}

}
