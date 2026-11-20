/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>

namespace WebView {

// The directories under which the UI process opens a local file for a renderer: those that hold the user's own files,
// which are the home directory, the temporary directories, and mounted volumes. A path counts once its symbolic links
// are resolved, and nothing in the kernel's interface trees (/proc, /sys, /dev) is ever under a root.
class WEBVIEW_API LocalFileRoots {
public:
    static bool contains(StringView canonical_path);

    // Adds a root, as a test harness does for the tests it loads as local files.
    static void add(StringView directory);

    // Replaces every root, returning those it replaced, so that a test can put a file outside them.
    static Vector<ByteString> replace_for_testing(Vector<ByteString>);

    // What a file in the kernel's interface trees reads depends on the process that opens it, which is the UI process
    // here, and a device node or a descriptor link there reaches whatever is behind it.
    static bool is_in_kernel_interface_tree(StringView canonical_path);
};

}
