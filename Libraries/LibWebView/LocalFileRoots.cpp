/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/LexicalPath.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/LocalFileRoots.h>
#include <stdlib.h>

namespace WebView {

static bool is_within(StringView directory, StringView path)
{
    if (path == directory)
        return true;
    if (!path.starts_with(directory))
        return false;
    auto separator = path[directory.length()];
#if defined(AK_OS_WINDOWS)
    return separator == '/' || separator == '\\';
#else
    return separator == '/';
#endif
}

// A root is a directory by its canonical path. The file system's root directory is never one: Under it is everything.
static Optional<ByteString> canonical_directory(StringView path)
{
    if (path.is_empty())
        return {};
    auto canonical_path = FileSystem::real_path(path);
    if (canonical_path.is_error() || LexicalPath { canonical_path.value() }.parts_view().is_empty())
        return {};
    if (LocalFileRoots::is_in_kernel_interface_tree(canonical_path.value()) || !FileSystem::is_directory(canonical_path.value()))
        return {};
    return canonical_path.release_value();
}

static void add_directory(Vector<ByteString>& directories, StringView path)
{
    auto directory = canonical_directory(path);
    if (directory.has_value() && !directories.contains_slow(*directory))
        directories.append(directory.release_value());
}

#if !defined(AK_OS_WINDOWS)
// A volume mounted after startup is under one of these, so one that isn't there yet still counts.
static void add_mount_point(Vector<ByteString>& directories, StringView path)
{
    if (FileSystem::exists(path))
        add_directory(directories, path);
    else
        directories.append(path);
}
#endif

static Vector<ByteString> initial_roots()
{
    Vector<ByteString> roots;
    add_directory(roots, Core::StandardPaths::home_directory());
    add_directory(roots, Core::StandardPaths::tempfile_directory());
    if (auto const* temporary_directory = getenv("TMPDIR"))
        add_directory(roots, StringView { temporary_directory, strlen(temporary_directory) });
#if defined(AK_OS_MACOS)
    add_mount_point(roots, "/Volumes"sv);
#elif !defined(AK_OS_WINDOWS)
    for (auto mount_point : { "/media"sv, "/run/media"sv, "/mnt"sv })
        add_mount_point(roots, mount_point);
#endif
    return roots;
}

// NB: The UI process checks the roots off its main thread too, when it reads the file a navigation names.
static Mutex& roots_mutex()
{
    static NeverDestroyed<Mutex> mutex;
    return *mutex;
}

static Vector<ByteString>& roots()
{
    static NeverDestroyed<Vector<ByteString>> roots { initial_roots() };
    return *roots;
}

bool LocalFileRoots::contains(StringView canonical_path)
{
    if (is_in_kernel_interface_tree(canonical_path))
        return false;
    MutexLocker locker(roots_mutex());
    return any_of(roots(), [&](auto const& root) { return is_within(root, canonical_path); });
}

void LocalFileRoots::add(StringView directory)
{
    Vector<ByteString> directories;
    add_directory(directories, directory);
    MutexLocker locker(roots_mutex());
    for (auto& added_directory : directories) {
        if (!roots().contains_slow(added_directory))
            roots().append(move(added_directory));
    }
}

Vector<ByteString> LocalFileRoots::replace_for_testing(Vector<ByteString> replacement)
{
    MutexLocker locker(roots_mutex());
    return exchange(roots(), move(replacement));
}

bool LocalFileRoots::is_in_kernel_interface_tree(StringView canonical_path)
{
#if defined(AK_OS_WINDOWS)
    (void)canonical_path;
    return false;
#else
    return any_of(Array { "/proc"sv, "/sys"sv, "/dev"sv }, [&](auto tree) { return is_within(tree, canonical_path); });
#endif
}

}
