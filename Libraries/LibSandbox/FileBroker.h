/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibSandbox/Sandbox.h>
#include <limits.h>
#include <sys/stat.h>

namespace Sandbox {

namespace Detail {

static constexpr u32 file_broker_magic = 0x4c424652u;

enum class FileBrokerOperation : u32 {
    Open,
    Access,
    Stat,
    SymlinkStat,
    ReadLink,
    MakeDirectory,
    RemoveFile,
    RemoveDirectory,
    Rename,
    // The address travels in the first path field, and the socket as the first descriptor.
    BindSocket,
};

struct FileBrokerRequest {
    u32 magic;
    u32 operation;
    i32 flags;
    u32 mode;
    u32 path_length;
    u32 second_path_length;
    // Bit 0 says a descriptor for resolving the first path was attached, bit 1 the same for the second path.
    u32 directory_fd_mask;
    char path[PATH_MAX];
    char second_path[PATH_MAX];
};

struct FileBrokerResponse {
    u32 magic;
    i32 error;
    u32 link_length;
    struct stat stat_result;
    char link[PATH_MAX];
};

// The socket that the SIGSYS handler sends its requests to.
int file_broker_socket();

// Sends every system call that takes a path to the broker or refuses it, and allows everything else, so that the helper
// is confined from the moment the broker runs. The helper's own policy stacks on top of this when it installs it.
[[nodiscard]] ErrorOr<void> install_file_system_filter();

}

// A path rule after its symbolic links were followed. The two paths are the same if there were none.
struct ResolvedPathRule {
    ByteString requested_path;
    ByteString resolved_path;
    PathRule::Access access { PathRule::Access::ReadOnly };
    bool is_directory { false };
};

enum class FileBrokerAccess {
    // Knowing that the path exists, which every directory above an allowed path needs for its path to be resolved.
    Metadata,
    Read,
    Write,
    // Making, removing or renaming the entry itself, which only a writable directory above it allows.
    ChangeEntry,
};

// Decides on canonical paths, which contain no symbolic links.
[[nodiscard]] bool file_broker_allows(ReadonlySpan<ResolvedPathRule>, StringView canonical_path, FileBrokerAccess);

// Seccomp can't read the paths that a process passes to the kernel, so it can't tell which files a process opens. On a
// kernel without Landlock, the seccomp policy instead traps every system call that takes a
// path, and a process forked from the helper does the work if the path is one the helper may use. The helper must have
// only one thread, and must not have installed its seccomp policy yet. The broker exits when the helper does.
[[nodiscard]] ErrorOr<void> start_file_broker(ReadonlySpan<PathRule>);

// Tells the seccomp policy to trap system calls that take a path, in place of allowing them.
[[nodiscard]] bool file_broker_is_active();

}
