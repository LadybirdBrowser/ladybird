/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <Compositor/Sandbox.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>

namespace Compositor {

ErrorOr<void> restrict_filesystem(StringView resource_root)
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    Vector<Sandbox::LandlockPath> paths;
    TRY(Sandbox::add_landlock_path_if_exists(paths, TRY(String::formatted("{}/fonts", resource_root)), Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_gpu_driver_landlock_paths(paths));

    TRY(Sandbox::restrict_filesystem_with_landlock(paths.span()));
    return {};
}

ErrorOr<void> apply_sandbox(StringView, StringView, StringView)
{
    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_filesystem_writes();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_socket_pairs();
    policy.allow_gpu_device_operations();
    policy.allow_common_runtime();
    policy.allow_executable_memory_mappings();
    // Some GPU drivers allocate writable executable code heaps lazily after
    // context creation, including while handling WebGL commands.
    policy.allow_writable_executable_memory_mappings();
    TRY(policy.install());

    return {};
}

}
