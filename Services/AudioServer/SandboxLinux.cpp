/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AudioServer/Sandbox.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>

namespace AudioServer {

ErrorOr<void> apply_sandbox(StringView)
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    auto executable_path = TRY(Core::System::current_executable_path());
    auto build_root = LexicalPath::dirname(LexicalPath::dirname(executable_path));

    Vector<Sandbox::LandlockPath> paths;
    // cpptrace opens loaded ELF objects when symbolizing in-process stack traces.
    TRY(Sandbox::add_landlock_path_if_exists(paths, executable_path, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, LexicalPath::join(build_root, "lib"sv).string(), Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/proc/self"sv, Sandbox::LandlockPath::Access::ReadOnly));

    // libpulse still keeps its cookie and runtime files in these, so they stay readable.
    auto pulse_runtime_path = LexicalPath::join(TRY(Core::StandardPaths::runtime_directory()), "pulse"sv).string();
    TRY(Core::Directory::create(pulse_runtime_path, Core::Directory::CreateDirectories::Yes, 0700));
    TRY(Sandbox::add_landlock_path_if_exists(paths, pulse_runtime_path, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, LexicalPath::join(Core::StandardPaths::config_directory(), "pulse"sv).string(), Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::restrict_filesystem_with_landlock(paths.span()));

    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_socket_pairs();
    policy.broker_unix_socket_connections();
    policy.allow_pulseaudio_client_file_operations();
    policy.allow_common_runtime();
    TRY(policy.install());
    return {};
}

}
