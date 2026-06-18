/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <LibWebView/Utilities.h>
#include <Services/RendererSandbox.h>

namespace RendererSandbox {

ErrorOr<void> apply_sandbox(Optional<StringView> config_path)
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    auto executable_path = TRY(Core::System::current_executable_path());
    auto build_root = LexicalPath::dirname(LexicalPath::dirname(executable_path));

    Vector<Sandbox::LandlockPath> paths;
    TRY(Sandbox::add_landlock_path_if_exists(paths, WebView::s_ladybird_resource_root, Sandbox::LandlockPath::Access::ReadOnly));
    if (config_path.has_value())
        TRY(Sandbox::add_landlock_path_if_exists(paths, *config_path, Sandbox::LandlockPath::Access::ReadOnly));
    // cpptrace opens loaded ELF objects when symbolizing in-process stack traces.
    TRY(Sandbox::add_landlock_path_if_exists(paths, executable_path, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, LexicalPath::join(build_root, "lib"sv).string(), Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/proc/self"sv, Sandbox::LandlockPath::Access::ReadOnly));
    for (auto const& path : TRY(Gfx::FontDatabase::font_directories()))
        TRY(Sandbox::add_landlock_path_if_exists(paths, path, Sandbox::LandlockPath::Access::ReadOnly));

    if (auto cranelift_compiler_path = Core::Environment::get("LADYBIRD_CRANELIFT_COMPILER"sv); cranelift_compiler_path.has_value()) {
        TRY(Sandbox::add_landlock_path_if_exists(paths, *cranelift_compiler_path, Sandbox::LandlockPath::Access::ReadAndExecute));
    } else {
        auto default_cranelift_compiler_path = LexicalPath::join(build_root, "bin/cranelift-compiler"sv).string();
        TRY(Sandbox::add_landlock_path_if_exists(paths, default_cranelift_compiler_path, Sandbox::LandlockPath::Access::ReadAndExecute));
    }

    auto pulse_runtime_path = LexicalPath::join(TRY(Core::StandardPaths::runtime_directory()), "pulse"sv).string();
    TRY(Core::Directory::create(pulse_runtime_path, Core::Directory::CreateDirectories::Yes, 0700));
    TRY(Sandbox::add_landlock_path_if_exists(paths, pulse_runtime_path, Sandbox::LandlockPath::Access::ReadWrite));
    TRY(Sandbox::add_landlock_path_if_exists(paths, LexicalPath::join(Core::StandardPaths::config_directory(), "pulse"sv).string(), Sandbox::LandlockPath::Access::ReadOnly));

    TRY(Sandbox::restrict_filesystem_with_landlock(paths.span()));

    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_filesystem_writes();
    policy.allow_file_descriptor_operations();
    policy.allow_process_creation();
    policy.allow_ipc();
    policy.allow_common_runtime();
    policy.allow_executable_memory_mappings();
    TRY(policy.install());

    return {};
}

}
