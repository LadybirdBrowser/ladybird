/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <VideoDecoderServer/Sandbox.h>

namespace VideoDecoderServer {

ErrorOr<void> restrict_filesystem(bool allow_gpu_driver)
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
    if (allow_gpu_driver)
        TRY(Sandbox::add_gpu_driver_landlock_paths(paths));

    TRY(Sandbox::restrict_filesystem_with_landlock(paths.span()));
    return {};
}

// FIXME: Narrow the GPU access to what decoding needs, once hardware decoders run here.
ErrorOr<void> apply_sandbox(StringView)
{
    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_socket_pairs();
    policy.allow_common_runtime();
    // The driver opens its device files for writing once decoding starts. Landlock limits writes to those.
    policy.allow_filesystem_writes();
    policy.allow_gpu_device_operations();
    policy.allow_executable_memory_mappings();
    policy.allow_writable_executable_memory_mappings();
    TRY(policy.install());

    return {};
}

}
