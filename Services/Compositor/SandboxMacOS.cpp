/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <Compositor/Sandbox.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibSandbox/Sandbox.h>
#include <LibWebView/Utilities.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

namespace Compositor {

ErrorOr<void> apply_sandbox()
{
    TRY(Sandbox::configure_runtime());

    auto executable_path = TRY(Core::System::current_executable_path());
    auto build_root = LexicalPath::dirname(LexicalPath::dirname(LexicalPath::dirname(LexicalPath::dirname(LexicalPath::dirname(executable_path)))));

    Vector<Sandbox::SeatbeltPath> paths;
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, executable_path, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, LexicalPath::join(build_root, "bin"sv).string(), Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, LexicalPath::join(build_root, "lib"sv).string(), Sandbox::SeatbeltPath::Access::ReadAndExecute));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, LexicalPath::join(build_root, "vcpkg_installed"sv).string(), Sandbox::SeatbeltPath::Access::ReadAndExecute));

    for (auto const& path : TRY(Gfx::FontDatabase::font_directories()))
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, path, Sandbox::SeatbeltPath::Access::ReadOnly));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, TRY(String::formatted("{}/fonts", WebView::s_ladybird_resource_root)), Sandbox::SeatbeltPath::Access::ReadOnly));

    auto cache_path = Core::StandardPaths::cache_directory();
    auto skia_cache_path = TRY(String::formatted("{}/Ladybird", cache_path));
    TRY(Core::Directory::create(skia_cache_path.to_byte_string(), Core::Directory::CreateDirectories::Yes));
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, skia_cache_path, Sandbox::SeatbeltPath::Access::ReadWrite));

    char darwin_user_cache_directory[PATH_MAX];
    if (confstr(_CS_DARWIN_USER_CACHE_DIR, darwin_user_cache_directory, sizeof(darwin_user_cache_directory)) > 0) {
        StringView darwin_user_cache_directory_view { darwin_user_cache_directory, strlen(darwin_user_cache_directory) };
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, darwin_user_cache_directory_view, Sandbox::SeatbeltPath::Access::ReadWrite));
        if (darwin_user_cache_directory_view.starts_with("/var/"sv)) {
            auto private_darwin_user_cache_directory = TRY(String::formatted("/private{}", darwin_user_cache_directory));
            TRY(Sandbox::add_seatbelt_path_if_exists(paths, private_darwin_user_cache_directory, Sandbox::SeatbeltPath::Access::ReadWrite));
        }
    }

    return Sandbox::apply_macos_sandbox(paths.span(), Sandbox::NetworkAccess::Denied);
}

}
