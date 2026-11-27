/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/WebView/Utilities.h>

// On macOS, the helpers are in the app bundle, which this test isn't a part of. Elsewhere, the build puts them where
// the browser looks for them from its own directory, which is this test's directory too.
#if !defined(AK_OS_MACOS)
TEST_CASE(each_helper_process_is_at_one_of_its_paths)
{
    for (auto process_name : { "ladybird-gpu"sv, "ladybird-image"sv, "ladybird-net"sv, "ladybird-probe"sv, "ladybird-render"sv }) {
        auto paths = TRY_OR_FAIL(WebView::get_paths_for_helper_process(process_name));
        if (!any_of(paths, [](auto const& path) { return FileSystem::exists(path); }))
            FAIL(ByteString::formatted("No file at any path for {}: {}", process_name, paths));
    }
}
#endif
