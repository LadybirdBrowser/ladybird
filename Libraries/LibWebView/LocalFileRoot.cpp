/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/LexicalPath.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/LocalFileRoot.h>

namespace WebView {

// NB: Every path here is canonical, and so absolute: is_child_of() takes any relative path to be under a directory.
bool LocalFileRoot::covers(StringView canonical_path) const
{
    if (scope == Scope::File)
        return canonical_path == path;
    return LexicalPath { canonical_path }.is_child_of(LexicalPath { path });
}

static ByteString resolved_directory(ByteString directory)
{
    if (auto resolved = FileSystem::real_path(directory); !resolved.is_error())
        return resolved.release_value();
    return LexicalPath::canonicalized_path(move(directory));
}

LocalFileGrantPolicy::LocalFileGrantPolicy(ByteString home_directory, Vector<ByteString> profile_directories, Optional<ByteString> root_for_tests)
    : m_home_directory(resolved_directory(move(home_directory)))
{
    for (auto& directory : profile_directories)
        m_profile_directories.append(resolved_directory(move(directory)));
    if (root_for_tests.has_value())
        m_root_for_tests = resolved_directory(root_for_tests.release_value());
}

bool LocalFileGrantPolicy::may_grant_directory(StringView canonical_directory) const
{
    LexicalPath directory { canonical_directory };
    if (directory.parts_view().is_empty() || LexicalPath { m_home_directory }.is_child_of(directory))
        return false;
    return !any_of(m_profile_directories, [&](auto const& profile_directory) {
        LexicalPath protected_directory { profile_directory };
        return protected_directory.is_child_of(directory) || directory.is_child_of(protected_directory);
    });
}

LocalFileRoot LocalFileGrantPolicy::root_for_opened_file(ByteString const& canonical_file_path) const
{
    if (m_root_for_tests.has_value() && LexicalPath { canonical_file_path }.is_child_of(LexicalPath { *m_root_for_tests }))
        return { *m_root_for_tests, LocalFileRoot::Scope::Directory };

    auto directory = LexicalPath::dirname(canonical_file_path);
    if (may_grant_directory(directory))
        return { move(directory), LocalFileRoot::Scope::Directory };
    return { canonical_file_path, LocalFileRoot::Scope::File };
}

Optional<LocalFileRoot> LocalFileGrantPolicy::restore(LocalFileRoot root) const
{
    if (root.scope == LocalFileRoot::Scope::File || root.path == m_root_for_tests || may_grant_directory(root.path))
        return root;
    return {};
}

}
