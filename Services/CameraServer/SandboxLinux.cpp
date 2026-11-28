/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <CameraServer/Sandbox.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>

namespace CameraServer {

ErrorOr<void> apply_sandbox(StringView)
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    auto executable_path = TRY(Core::System::current_executable_path());
    auto build_root = LexicalPath::dirname(LexicalPath::dirname(executable_path));

    Vector<Sandbox::PathRule> paths;
    // cpptrace opens loaded ELF objects when symbolizing in-process stack traces.
    TRY(Sandbox::add_path_if_exists(paths, executable_path, Sandbox::PathRule::Access::ReadOnly));
    TRY(Sandbox::add_path_if_exists(paths, LexicalPath::join(build_root, "lib"sv).string(), Sandbox::PathRule::Access::ReadOnly));
    // PipeWire's modules and plugins are among these: it loaded them before this, and looks for them again after.
    TRY(Sandbox::add_loaded_library_directories(paths, build_root));
    TRY(Sandbox::add_path_if_exists(paths, "/proc/self"sv, Sandbox::PathRule::Access::ReadOnly));
    // PipeWire's library reads its configuration whenever it is set up, which is once the sandbox is on.
    for (auto directory : { "/usr/share/pipewire"sv, "/usr/local/share/pipewire"sv, "/etc/pipewire"sv })
        TRY(Sandbox::add_path_if_exists(paths, directory, Sandbox::PathRule::Access::ReadOnly));
    TRY(Sandbox::add_path_if_exists(paths, LexicalPath::join(Core::StandardPaths::config_directory(), "pipewire"sv).string(), Sandbox::PathRule::Access::ReadOnly));
    TRY(Sandbox::restrict_filesystem(paths.span()));

    // No camera is opened here: the pictures come over the one socket the broker connects, in memory the daemon shares.
    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_epoll();
    policy.allow_socket_pairs();
    policy.broker_unix_socket_connections();
    // PipeWire names its threads by writing to /proc, and carries on when it can't.
    policy.refuse_writable_file_opens();
    policy.allow_common_runtime();
    TRY(policy.install());
    return {};
}

}
