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
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/lib"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/lib64"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/lib"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/local/lib"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/glvnd"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/share/glvnd"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/share/drirc.d"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/share/vulkan"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/dev/dri"sv, Sandbox::LandlockPath::Access::ReadWrite));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/dev/udmabuf"sv, Sandbox::LandlockPath::Access::ReadWrite));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/sys"sv, Sandbox::LandlockPath::Access::ReadOnly));
    if (auto library_path = Core::Environment::get("LD_LIBRARY_PATH"sv); library_path.has_value()) {
        for (auto path : library_path->split_view(':'))
            TRY(Sandbox::add_landlock_path_if_exists(paths, path, Sandbox::LandlockPath::Access::ReadOnly));
    }

    TRY(Sandbox::add_landlock_path_if_exists(paths, "/dev/nvidiactl"sv, Sandbox::LandlockPath::Access::ReadWrite));

    // NB: Add all of the primary nvidia device files (e.g. /dev/nvidia0, /dev/nvidia1, etc).
    auto flags = static_cast<Core::DirIterator::Flags>(Core::DirIterator::SkipDots | Core::DirIterator::NoStat);
    TRY(Core::Directory::for_each_entry("/dev"sv, flags, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<IterationDecision> {
        if (entry.name.starts_with("nvidia"sv)) {
            auto suffix = entry.name.substring_view(6);
            if (!suffix.is_empty() && all_of(suffix, is_ascii_digit))
                TRY(Sandbox::add_landlock_path_if_exists(paths, TRY(String::formatted("/dev/{}", entry.name)), Sandbox::LandlockPath::Access::ReadWrite));
        }

        return IterationDecision::Continue;
    }));

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
    policy.allow_executable_memory_mappings();
    // Some GPU drivers allocate writable executable code heaps lazily after
    // context creation, including while handling WebGL commands.
    policy.allow_writable_executable_memory_mappings();
    TRY(policy.install());

    return {};
}

}
