/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>

namespace WebView {

struct WEBVIEW_API LocalFileRoot {
    enum class Scope : u8 {
        Directory,
        File,
    };

    ByteString path;
    Scope scope { Scope::Directory };

    bool covers(StringView canonical_path) const;
    bool operator==(LocalFileRoot const&) const = default;
};

struct UserSelectedLocalFile { };

// What a navigation to a file: URL may read: nothing, the file the user chose, or what one of the roots covers.
using LocalFileAccess = Variant<Empty, UserSelectedLocalFile, Vector<LocalFileRoot>>;

// A file the user opens grants its folder, or the file alone when the folder is too wide: a root never contains a
// protected directory (the profile's, or the home directory), nor is it inside one other than home.
class WEBVIEW_API LocalFileGrantPolicy {
public:
    LocalFileGrantPolicy() = default;
    LocalFileGrantPolicy(ByteString home_directory, Vector<ByteString> profile_directories, Optional<ByteString> root_for_tests = {});

    LocalFileRoot root_for_opened_file(ByteString const& canonical_file_path) const;

    // A saved root, if it's still allowed. A disallowed directory root is dropped, since narrowing it to a file root
    // wouldn't keep access to its contents.
    Optional<LocalFileRoot> restore(LocalFileRoot) const;

private:
    bool may_grant_directory(StringView canonical_directory) const;

    ByteString m_home_directory;
    Vector<ByteString> m_profile_directories;
    Optional<ByteString> m_root_for_tests;
};

}
