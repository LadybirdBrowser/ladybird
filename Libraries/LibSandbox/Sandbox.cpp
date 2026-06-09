/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibSandbox/Sandbox.h>

#if defined(AK_OS_LINUX)
#    include <AK/LexicalPath.h>
#    include <AK/ScopeGuard.h>
#    include <AK/Vector.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <linux/landlock.h>
#    include <sys/prctl.h>
#    include <sys/stat.h>
#    include <sys/syscall.h>
#    include <unistd.h>
#endif

#if defined(__GLIBC__)
#    include <malloc.h>
#endif

namespace Sandbox {

ErrorOr<void> install_no_new_privileges()
{
#if defined(AK_OS_LINUX)
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
        return Error::from_syscall("prctl(PR_SET_NO_NEW_PRIVS)"sv, errno);
#endif
    return {};
}

ErrorOr<void> configure_runtime()
{
#if defined(AK_OS_LINUX) && defined(__GLIBC__) && defined(M_ARENA_MAX) && !defined(HAS_ADDRESS_SANITIZER)
    if (mallopt(M_ARENA_MAX, 4) == 0)
        return Error::from_string_literal("mallopt(M_ARENA_MAX) failed");
#endif
    return {};
}

ErrorOr<void> add_landlock_path_if_exists(Vector<LandlockPath>& paths, StringView path, LandlockPath::Access access)
{
#if defined(AK_OS_LINUX)
    auto path_bytes = path.to_byte_string();

    struct stat statbuf;
    if (stat(path_bytes.characters(), &statbuf) < 0) {
        if (errno == ENOENT)
            return {};
        return Error::from_syscall("stat"sv, errno);
    }

    if (!S_ISDIR(statbuf.st_mode))
        path_bytes = LexicalPath::dirname(path_bytes);

    for (auto const& existing_path : paths) {
        if (existing_path.access == access && existing_path.path == path_bytes)
            return {};
    }

    TRY(paths.try_append({ move(path_bytes), access }));
#else
    (void)paths;
    (void)path;
    (void)access;
#endif
    return {};
}

ErrorOr<void> restrict_filesystem_with_landlock(ReadonlySpan<LandlockPath> paths)
{
#if defined(AK_OS_LINUX) && defined(__NR_landlock_create_ruleset) && defined(__NR_landlock_add_rule) && defined(__NR_landlock_restrict_self)
    auto landlock_abi = syscall(__NR_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (landlock_abi < 0) {
        if (errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL)
            return {};
        return Error::from_syscall("landlock_create_ruleset(LANDLOCK_CREATE_RULESET_VERSION)"sv, errno);
    }
    if (landlock_abi == 0)
        return {};

    landlock_ruleset_attr ruleset_attributes {};
    ruleset_attributes.handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE
        | LANDLOCK_ACCESS_FS_WRITE_FILE
        | LANDLOCK_ACCESS_FS_READ_FILE
        | LANDLOCK_ACCESS_FS_READ_DIR
        | LANDLOCK_ACCESS_FS_REMOVE_DIR
        | LANDLOCK_ACCESS_FS_REMOVE_FILE
        | LANDLOCK_ACCESS_FS_MAKE_CHAR
        | LANDLOCK_ACCESS_FS_MAKE_DIR
        | LANDLOCK_ACCESS_FS_MAKE_REG
        | LANDLOCK_ACCESS_FS_MAKE_SOCK
        | LANDLOCK_ACCESS_FS_MAKE_FIFO
        | LANDLOCK_ACCESS_FS_MAKE_BLOCK
        | LANDLOCK_ACCESS_FS_MAKE_SYM;

#    ifdef LANDLOCK_ACCESS_FS_REFER
    if (landlock_abi >= 2)
        ruleset_attributes.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
#    endif
#    ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (landlock_abi >= 3)
        ruleset_attributes.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
#    endif
#    if defined(LANDLOCK_ACCESS_NET_BIND_TCP) && defined(LANDLOCK_ACCESS_NET_CONNECT_TCP)
    auto ruleset_attributes_size = offsetof(landlock_ruleset_attr, handled_access_net);
#    else
    auto ruleset_attributes_size = sizeof(ruleset_attributes);
#    endif
    auto ruleset_fd = syscall(__NR_landlock_create_ruleset, &ruleset_attributes, ruleset_attributes_size, 0);
    if (ruleset_fd < 0)
        return Error::from_syscall("landlock_create_ruleset"sv, errno);

    ArmedScopeGuard close_ruleset_fd = [&] {
        close(static_cast<int>(ruleset_fd));
    };

    for (auto const& landlock_path : paths) {
        auto path_fd = open(landlock_path.path.characters(), O_PATH | O_CLOEXEC);
        if (path_fd < 0)
            return Error::from_syscall("open(O_PATH)"sv, errno);

        ArmedScopeGuard close_path_fd = [&] {
            close(path_fd);
        };

        landlock_path_beneath_attr path_beneath {};
        path_beneath.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
        if (landlock_path.access == LandlockPath::Access::ReadAndExecute)
            path_beneath.allowed_access |= LANDLOCK_ACCESS_FS_EXECUTE;
        if (landlock_path.access == LandlockPath::Access::ReadWrite) {
            path_beneath.allowed_access |= LANDLOCK_ACCESS_FS_WRITE_FILE
                | LANDLOCK_ACCESS_FS_REMOVE_DIR
                | LANDLOCK_ACCESS_FS_REMOVE_FILE
                | LANDLOCK_ACCESS_FS_MAKE_DIR
                | LANDLOCK_ACCESS_FS_MAKE_REG
                | LANDLOCK_ACCESS_FS_MAKE_SOCK
                | LANDLOCK_ACCESS_FS_MAKE_FIFO;
#    ifdef LANDLOCK_ACCESS_FS_REFER
            if (landlock_abi >= 2)
                path_beneath.allowed_access |= LANDLOCK_ACCESS_FS_REFER;
#    endif
#    ifdef LANDLOCK_ACCESS_FS_TRUNCATE
            if (landlock_abi >= 3)
                path_beneath.allowed_access |= LANDLOCK_ACCESS_FS_TRUNCATE;
#    endif
        }
        path_beneath.parent_fd = path_fd;
        if (syscall(__NR_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_beneath, 0) < 0)
            return Error::from_syscall("landlock_add_rule"sv, errno);
    }

    if (syscall(__NR_landlock_restrict_self, ruleset_fd, 0) < 0)
        return Error::from_syscall("landlock_restrict_self"sv, errno);
#else
    (void)paths;
#endif

    return {};
}

ErrorOr<void> restrict_filesystem_with_landlock(ReadonlySpan<StringView> readable_paths)
{
    Vector<LandlockPath> paths;
    for (auto readable_path : readable_paths)
        TRY(paths.try_append({ readable_path.to_byte_string(), LandlockPath::Access::ReadOnly }));
    return restrict_filesystem_with_landlock(paths.span());
}

}
