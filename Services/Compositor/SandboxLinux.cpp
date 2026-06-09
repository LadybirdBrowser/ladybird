/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/Sandbox.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/StandardPaths.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <LibWebView/Utilities.h>

namespace Compositor {

ErrorOr<void> apply_sandbox()
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    Vector<Sandbox::LandlockPath> paths;
    for (auto const& path : TRY(Gfx::FontDatabase::font_directories()))
        TRY(Sandbox::add_landlock_path_if_exists(paths, path, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, TRY(String::formatted("{}/fonts", WebView::s_ladybird_resource_root)), Sandbox::LandlockPath::Access::ReadOnly));

    auto mesa_shader_cache_path = Core::Environment::get("MESA_SHADER_CACHE_DIR"sv)
                                      .map([](auto path) { return path.to_byte_string(); })
                                      .value_or_lazy_evaluated([] { return ByteString::formatted("{}/mesa_shader_cache", Core::StandardPaths::cache_directory()); });
    TRY(Core::Directory::create(mesa_shader_cache_path, Core::Directory::CreateDirectories::Yes));
    TRY(Sandbox::add_landlock_path_if_exists(paths, mesa_shader_cache_path, Sandbox::LandlockPath::Access::ReadWrite));

    TRY(Sandbox::restrict_filesystem_with_landlock(paths.span()));

    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_filesystem_writes();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_gpu_device_operations();
    policy.allow_common_runtime();
    TRY(policy.install());

    return {};
}

}
