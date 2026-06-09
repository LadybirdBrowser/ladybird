/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <Compositor/Sandbox.h>
#include <LibCore/System.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <LibWebView/Utilities.h>

namespace Compositor {

ErrorOr<void> apply_sandbox()
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    Vector<String> readable_path_strings;
    for (auto const& path : TRY(Gfx::FontDatabase::font_directories())) {
        if (auto result = Core::System::stat(path); result.is_error()) {
            if (result.error().is_errno() && result.error().code() == ENOENT)
                continue;
            return result.release_error();
        }
        readable_path_strings.append(path);
    }
    readable_path_strings.append(TRY(String::formatted("{}/fonts", WebView::s_ladybird_resource_root)));

    Vector<StringView> readable_paths;
    for (auto const& path : readable_path_strings)
        readable_paths.append(path.bytes_as_string_view());
    TRY(Sandbox::restrict_filesystem_with_landlock(readable_paths.span()));

    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_gpu_device_operations();
    policy.allow_common_runtime();
    TRY(policy.install());

    return {};
}

}
