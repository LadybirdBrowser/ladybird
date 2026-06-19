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

#if defined(AK_OS_MACOS)
#    include <AK/LexicalPath.h>
#    include <AK/StringBuilder.h>
#    include <errno.h>
#    include <limits.h>
#    include <sandbox.h>
#    include <signal.h>
#    include <stdlib.h>
#    include <sys/stat.h>
#    include <unistd.h>

extern "C" {
int sandbox_init_with_parameters(char const* profile, u64 flags, char const* const parameters[], char** errorbuf);
}
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

#if defined(AK_OS_LINUX)
ErrorOr<void> add_landlock_path_if_exists(Vector<LandlockPath>& paths, StringView path, LandlockPath::Access access)
{
    auto path_bytes = path.to_byte_string();

    struct stat statbuf;
    if (stat(path_bytes.characters(), &statbuf) < 0) {
        if (errno == ENOENT)
            return {};
        return Error::from_syscall("stat"sv, errno);
    }

    bool is_directory = S_ISDIR(statbuf.st_mode);

    for (auto const& existing_path : paths) {
        if (existing_path.access == access && existing_path.path == path_bytes)
            return {};
    }

    TRY(paths.try_append({ move(path_bytes), access, is_directory }));
    return {};
}
#endif

#if defined(AK_OS_MACOS)
ErrorOr<void> add_seatbelt_path_if_exists(Vector<SeatbeltPath>& paths, StringView path, SeatbeltPath::Access access)
{
    auto path_bytes = path.to_byte_string();

    struct stat statbuf;
    if (stat(path_bytes.characters(), &statbuf) < 0) {
        if (errno == ENOENT)
            return {};
        return Error::from_syscall("stat"sv, errno);
    }

    char resolved_path[PATH_MAX];
    if (realpath(path_bytes.characters(), resolved_path) == nullptr)
        return Error::from_syscall("realpath"sv, errno);
    path_bytes = resolved_path;

    auto is_directory = S_ISDIR(statbuf.st_mode);

    for (auto const& existing_path : paths) {
        if (existing_path.access == access && existing_path.path == path_bytes)
            return {};
    }

    TRY(paths.try_append({ move(path_bytes), access, is_directory }));
    return {};
}

static void append_sandbox_string_literal(StringBuilder& builder, StringView string)
{
    builder.append('"');
    for (auto ch : string.bytes()) {
        if (ch == '"' || ch == '\\')
            builder.append('\\');
        builder.append(static_cast<char>(ch));
    }
    builder.append('"');
}

static void append_sandbox_path_filter(StringBuilder& builder, SeatbeltPath const& path)
{
    builder.append(path.is_directory ? "(subpath "sv : "(literal "sv);
    append_sandbox_string_literal(builder, path.path);
    builder.append(')');
}

static bool seatbelt_path_allows_access(SeatbeltPath::Access path_access, SeatbeltPath::Access requested_access)
{
    if (requested_access == SeatbeltPath::Access::ReadOnly)
        return true;
    return path_access == requested_access;
}

static ErrorOr<void> append_allowed_paths(StringBuilder& builder, StringView operation, ReadonlySpan<SeatbeltPath> paths, SeatbeltPath::Access access)
{
    bool emitted_header = false;
    for (auto const& path : paths) {
        if (!seatbelt_path_allows_access(path.access, access))
            continue;

        if (!emitted_header) {
            builder.append("(allow "sv);
            builder.append(operation);
            emitted_header = true;
        }
        builder.append(' ');
        append_sandbox_path_filter(builder, path);
    }

    if (emitted_header)
        builder.append(")\n"sv);

    return {};
}

static ErrorOr<void> append_allowed_path_extensions(StringBuilder& builder, ReadonlySpan<SeatbeltPath> paths, SeatbeltPath::Access access)
{
    auto extension_class = access == SeatbeltPath::Access::ReadWrite ? "com.apple.app-sandbox.read-write"sv : "com.apple.app-sandbox.read"sv;

    bool emitted_header = false;
    for (auto const& path : paths) {
        if (!seatbelt_path_allows_access(path.access, access))
            continue;

        if (!emitted_header) {
            builder.append("(allow file-issue-extension"sv);
            emitted_header = true;
        }
        builder.append(" (require-all (extension-class "sv);
        append_sandbox_string_literal(builder, extension_class);
        builder.append(") "sv);
        append_sandbox_path_filter(builder, path);
        builder.append(')');
    }

    if (emitted_header)
        builder.append(")\n"sv);

    return {};
}

static ErrorOr<void> append_allowed_executables(StringBuilder& builder, ReadonlySpan<ByteString> executable_paths)
{
    if (executable_paths.is_empty())
        return {};

    builder.append("(allow process-exec"sv);
    for (auto const& path : executable_paths) {
        builder.append(" (literal "sv);
        append_sandbox_string_literal(builder, path);
        builder.append(')');
    }
    builder.append(")\n"sv);

    return {};
}

static void sandbox_violation_signal_handler(int)
{
    char const message[] = "Sandbox violation: terminating process\n";
    [[maybe_unused]] auto nwritten = write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(128 + SIGSYS);
}

static ErrorOr<void> install_sandbox_violation_signal_handler()
{
    struct sigaction action {};
    action.sa_handler = sandbox_violation_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    if (sigaction(SIGSYS, &action, nullptr) < 0)
        return Error::from_syscall("sigaction(SIGSYS)"sv, errno);
    return {};
}

ErrorOr<void> apply_macos_sandbox(ReadonlySpan<SeatbeltPath> paths, NetworkAccess network_access, ReadonlySpan<ByteString> executable_paths)
{
    TRY(install_sandbox_violation_signal_handler());

    StringBuilder profile;
    TRY(profile.try_append(R"~~~(
(version 1)
(deny default
    (with send-signal SIGSYS)
    (with message "Ladybird macOS sandbox default deny"))

(allow process-info*)
(allow signal (target self))
(allow sysctl-read)
(allow system*)
(allow ipc*)
(allow mach*)
(allow iokit-open-user-client
    (iokit-user-client-class "IOSurfaceRootUserClient"))
(allow user-preference-read
    (preference-domain "kCFPreferencesAnyApplication")
    (preference-domain "org.ladybird.ladybird"))

(allow network-outbound
    (literal "/private/var/run/syslog"))

(deny syscall-unix
    (with send-signal SIGKILL)
    (with message "Ladybird macOS sandbox syscall deny"))

(allow syscall-unix
    (syscall-group-bsdthread)
    (syscall-group-close)
    (syscall-group-fcntl)
    (syscall-group-getfsstat)
    (syscall-group-kevent)
    (syscall-group-kqueue)
    (syscall-group-mkdir)
    (syscall-group-open)
    (syscall-group-open-dprotected)
    (syscall-group-pthread)
    (syscall-group-pthread-cv)
    (syscall-group-pthread-locks)
    (syscall-group-read)
    (syscall-group-recv)
    (syscall-group-rlimit)
    (syscall-group-select)
    (syscall-group-send)
    (syscall-group-signal)
    (syscall-group-sockopt)
    (syscall-group-stat)
    (syscall-group-statfs)
    (syscall-group-ulock)
    (syscall-group-write)
    (syscall-number
        SYS___disable_threadsignal
        SYS___channel_open
        SYS___mac_syscall
        SYS___semwait_signal
        SYS___semwait_signal_nocancel
        SYS_abort_with_payload
        SYS_access
        SYS_change_fdguard_np
        SYS_connect
        SYS_crossarch_trap
        SYS_csops_audittoken
        SYS_csrctl
        SYS_dup
        SYS_exit
        SYS_faccessat
        SYS_fileport_makefd
        SYS_fileport_makeport
        SYS_fgetattrlist
        SYS_fgetxattr
        SYS_flock
        SYS_fsgetpath
        SYS_fsync
        SYS_ftruncate
        SYS_getaudit_addr
        SYS_getattrlist
        SYS_getattrlistbulk
        SYS_getdirentries64
        SYS_getentropy
        SYS_getegid
        SYS_geteuid
        SYS_getgid
        SYS_gethostuuid
        SYS_getpeername
        SYS_getpid
        SYS_getrusage
        SYS_getsockname
        SYS_gettid
        SYS_gettimeofday
        SYS_getuid
        SYS_getxattr
        SYS_ioctl
        SYS_issetugid
        SYS_kdebug_trace
        SYS_kdebug_trace64
        SYS_kdebug_trace_string
        SYS_kdebug_typefilter
        SYS_listxattr
        SYS_lseek
        SYS_madvise
        SYS_mlock
        SYS_mmap
        SYS_mprotect
        SYS_mremap_encrypted
        SYS_msync
        SYS_munlock
        SYS_munmap
        SYS_necp_client_action
        SYS_necp_open
        SYS_open
        SYS_open_nocancel
        SYS_openat
        SYS_os_fault_with_payload
        SYS_pathconf
        SYS_pipe
        SYS_poll
        SYS_posix_spawn
        SYS_proc_info
        SYS_readlink
        SYS_rename
        SYS_rmdir
        SYS_sendfile
        SYS_shm_open
        SYS_shared_region_check_np
        SYS_shared_region_map_and_slide_2_np
        SYS_socket
        SYS_socketpair
        SYS_sysctl
        SYS_sysctlbyname
        SYS_thread_selfid
        SYS_umask
        SYS_wait4
        SYS_work_interval_ctl
        SYS_workq_kernreturn
        SYS_workq_open))

(allow file-read-metadata)
(allow file-read*
    (literal "/")
    (literal "/dev/dtracehelper")
    (literal "/dev/null")
    (literal "/dev/random")
    (literal "/dev/urandom")
    (literal "/private/etc/localtime")
    (subpath "/private/etc/ssl")
    (subpath "/System")
    (subpath "/Library/Preferences/Logging")
    (subpath "/private/var/db/timezone")
    (subpath "/usr/lib")
    (subpath "/usr/share"))

(allow file-map-executable
    (subpath "/System")
    (subpath "/usr/lib"))

(allow file-write-data file-ioctl
    (literal "/dev/dtracehelper"))
)~~~"sv));

    if (network_access == NetworkAccess::Allowed)
        TRY(profile.try_append("(allow network*)\n"sv));

    TRY(append_allowed_paths(profile, "file-read*"sv, paths, SeatbeltPath::Access::ReadOnly));
    TRY(append_allowed_paths(profile, "file-map-executable"sv, paths, SeatbeltPath::Access::ReadAndExecute));
    TRY(append_allowed_paths(profile, "file-write*"sv, paths, SeatbeltPath::Access::ReadWrite));
    TRY(append_allowed_path_extensions(profile, paths, SeatbeltPath::Access::ReadOnly));
    TRY(append_allowed_path_extensions(profile, paths, SeatbeltPath::Access::ReadWrite));
    TRY(append_allowed_executables(profile, executable_paths));

    auto profile_string = profile.to_byte_string();

    char* errorbuf = nullptr;
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    auto result = sandbox_init_with_parameters(profile_string.characters(), 0, nullptr, &errorbuf);
#    pragma clang diagnostic pop
    if (result < 0) {
        if (errorbuf) {
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
            sandbox_free_error(errorbuf);
#    pragma clang diagnostic pop
        }
        return Error::from_string_literal("sandbox_init_with_parameters failed");
    }
    return {};
}
#endif

#if defined(AK_OS_LINUX)
ErrorOr<void> restrict_filesystem_with_landlock(ReadonlySpan<LandlockPath> paths)
{
#    if defined(__NR_landlock_create_ruleset) && defined(__NR_landlock_add_rule) && defined(__NR_landlock_restrict_self)
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

#        ifdef LANDLOCK_ACCESS_FS_REFER
    if (landlock_abi >= 2)
        ruleset_attributes.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
#        endif
#        ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (landlock_abi >= 3)
        ruleset_attributes.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
#        endif
#        if defined(LANDLOCK_ACCESS_NET_BIND_TCP) && defined(LANDLOCK_ACCESS_NET_CONNECT_TCP)
    auto ruleset_attributes_size = offsetof(landlock_ruleset_attr, handled_access_net);
#        else
    auto ruleset_attributes_size = sizeof(ruleset_attributes);
#        endif
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
        switch (landlock_path.access) {
        case LandlockPath::Access::ReadOnly: {
            path_beneath.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE;

            if (landlock_path.is_directory)
                path_beneath.allowed_access |= LANDLOCK_ACCESS_FS_READ_DIR;
            break;
        }
        case LandlockPath::Access::ReadAndExecute: {
            path_beneath.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_EXECUTE;

            if (landlock_path.is_directory)
                path_beneath.allowed_access |= LANDLOCK_ACCESS_FS_READ_DIR;
            break;
        }
        case LandlockPath::Access::ReadWrite: {
            path_beneath.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE;

#        ifdef LANDLOCK_ACCESS_FS_TRUNCATE
            if (landlock_abi >= 3)
                path_beneath.allowed_access |= LANDLOCK_ACCESS_FS_TRUNCATE;
#        endif

            if (landlock_path.is_directory) {
                path_beneath.allowed_access |= LANDLOCK_ACCESS_FS_READ_DIR
                    | LANDLOCK_ACCESS_FS_REMOVE_DIR
                    | LANDLOCK_ACCESS_FS_REMOVE_FILE
                    | LANDLOCK_ACCESS_FS_MAKE_DIR
                    | LANDLOCK_ACCESS_FS_MAKE_REG
                    | LANDLOCK_ACCESS_FS_MAKE_SOCK
                    | LANDLOCK_ACCESS_FS_MAKE_FIFO;

#        ifdef LANDLOCK_ACCESS_FS_REFER
                if (landlock_abi >= 2)
                    path_beneath.allowed_access |= LANDLOCK_ACCESS_FS_REFER;
#        endif
            }
            break;
        }
        }
        path_beneath.parent_fd = path_fd;
        if (syscall(__NR_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_beneath, 0) < 0)
            return Error::from_syscall("landlock_add_rule"sv, errno);
    }

    if (syscall(__NR_landlock_restrict_self, ruleset_fd, 0) < 0)
        return Error::from_syscall("landlock_restrict_self"sv, errno);
#    else
    (void)paths;
#    endif

    return {};
}
#endif

}
