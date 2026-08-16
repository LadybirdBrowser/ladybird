/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <LibWasm/Types.h>
#include <WasmCompiler/Sandbox.h>

namespace WasmCompiler {

ErrorOr<void> apply_sandbox()
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    Vector<Sandbox::LandlockPath> paths;
    TRY(Sandbox::add_landlock_path_if_exists(paths, TRY(Core::System::current_executable_path()), Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, Wasm::cranelift_compiler_path(), Sandbox::LandlockPath::Access::ReadAndExecute));
#if ARCH(X86_64)
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/lib64/ld-linux-x86-64.so.2"sv, Sandbox::LandlockPath::Access::ReadAndExecute));
#elif ARCH(AARCH64)
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/lib/ld-linux-aarch64.so.1"sv, Sandbox::LandlockPath::Access::ReadAndExecute));
#elif ARCH(RISCV64)
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/lib/ld-linux-riscv64-lp64d.so.1"sv, Sandbox::LandlockPath::Access::ReadAndExecute));
#endif
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/lib"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/lib64"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/lib"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/local/lib"sv, Sandbox::LandlockPath::Access::ReadOnly));
    if (auto library_path = Core::Environment::get("LD_LIBRARY_PATH"sv); library_path.has_value()) {
        for (auto path : library_path->split_view(':'))
            TRY(Sandbox::add_landlock_path_if_exists(paths, path, Sandbox::LandlockPath::Access::ReadOnly));
    }
    TRY(Sandbox::restrict_filesystem_with_landlock(paths.span()));

    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_file_descriptor_operations();
    policy.allow_process_creation();
    policy.allow_ipc();
    policy.allow_common_runtime();
    policy.allow_executable_memory_mappings();
    TRY(policy.install());

    return {};
}

}
