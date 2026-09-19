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

// /etc, /tmp and /var are symbolic links into /private. Seatbelt checks some operations, such as file-test-existence,
// against the path before it follows those links, so a rule for a path in /private also has to name the short form.
static Optional<StringView> path_alias_outside_private(StringView path)
{
    for (auto prefix : { "/private/etc"sv, "/private/tmp"sv, "/private/var"sv }) {
        if (path == prefix || path.starts_with(ByteString::formatted("{}/", prefix)))
            return path.substring_view("/private"sv.length());
    }
    return {};
}

static void append_sandbox_path_filter(StringBuilder& builder, StringView filter, StringView path)
{
    builder.appendff("({} ", filter);
    append_sandbox_string_literal(builder, path);
    builder.append(')');

    if (auto alias = path_alias_outside_private(path); alias.has_value()) {
        builder.appendff(" ({} ", filter);
        append_sandbox_string_literal(builder, *alias);
        builder.append(')');
    }
}

static void append_sandbox_path_filter(StringBuilder& builder, SeatbeltPath const& path)
{
    append_sandbox_path_filter(builder, path.is_directory ? "subpath"sv : "literal"sv, path.path);
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

// Resolving a path, as realpath() does, needs the metadata of every directory above it.
static ErrorOr<void> append_allowed_ancestor_directories(StringBuilder& builder, ReadonlySpan<SeatbeltPath> paths)
{
    Vector<ByteString> ancestors;
    for (auto const& path : paths) {
        for (auto ancestor = LexicalPath::dirname(path.path); ancestor != "/"sv && !ancestor.is_empty(); ancestor = LexicalPath::dirname(ancestor)) {
            if (!ancestors.contains_slow(ancestor))
                TRY(ancestors.try_append(ancestor));
        }
    }

    if (ancestors.is_empty())
        return {};

    builder.append("(allow file-read-metadata file-test-existence"sv);
    for (auto const& ancestor : ancestors) {
        builder.append(' ');
        append_sandbox_path_filter(builder, "literal"sv, ancestor);
    }
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
        builder.append(") (require-any "sv);
        append_sandbox_path_filter(builder, path);
        builder.append("))"sv);
    }

    if (emitted_header)
        builder.append(")\n"sv);

    return {};
}

static ErrorOr<void> append_allowed_executables(StringBuilder& builder, ReadonlySpan<ByteString> executable_paths)
{
    if (executable_paths.is_empty())
        return {};

    builder.append("(allow process-fork)\n(allow process-exec"sv);
    for (auto const& path : executable_paths) {
        builder.append(" (literal "sv);
        append_sandbox_string_literal(builder, path);
        builder.append(')');
    }
    builder.append(")\n"sv);

    return {};
}

static ErrorOr<void> append_allowed_iokit_user_client_classes(StringBuilder& builder, ReadonlySpan<StringView> iokit_user_client_classes)
{
    for (auto const& user_client_class : iokit_user_client_classes) {
        builder.append("(allow iokit-open-user-client (iokit-user-client-class "sv);
        append_sandbox_string_literal(builder, user_client_class);
        builder.append("))\n"sv);
    }

    return {};
}

static ErrorOr<void> append_allowed_mach_services(StringBuilder& builder, SeatbeltProfile const& options)
{
    if (!options.mach_server_name.is_empty()) {
        builder.append("(allow mach-lookup (global-name "sv);
        append_sandbox_string_literal(builder, options.mach_server_name);
        builder.append("))\n"sv);
    }

    if (has_flag(options.system_services, SystemService::Fonts)) {
        builder.append(R"~~~(
(allow mach-lookup
    (global-name "com.apple.fonts")
    (global-name "com.apple.FontObjectsServer"))
)~~~"sv);
    }

    if (has_flag(options.system_services, SystemService::Audio)) {
        builder.append(R"~~~(
(allow mach-lookup
    (global-name "com.apple.audio.audiohald")
    (global-name "com.apple.audio.AudioComponentRegistrar")
    (global-name "com.apple.audio.AudioSession")
    (xpc-service-name "com.apple.audio.SandboxHelper"))
)~~~"sv);
    }

    if (has_flag(options.system_services, SystemService::VideoDecoding)) {
        builder.append(R"~~~(
(allow mach-lookup
    (xpc-service-name "com.apple.coremedia.videodecoder"))
)~~~"sv);
    }

    if (has_flag(options.system_services, SystemService::GPU)) {
        builder.append(R"~~~(
(allow mach-lookup
    (global-name "com.apple.CARenderServer")
    (xpc-service-name "com.apple.MTLCompilerService"))

; Metal loads the driver bundles for some GPUs from here.
(allow file-read* file-test-existence
    (subpath "/Library/GPUBundles"))

; ANGLE asks for the paths of its own descriptors while it sets up an EGL display.
(allow system-fcntl
    (fcntl-command F_GETPATH))
)~~~"sv);
    }

    return {};
}

ErrorOr<void> apply_macos_sandbox(SeatbeltProfile const& options)
{
    StringBuilder profile;
    TRY(profile.try_append(R"~~~(
(version 1)
(deny default
    (with message "Ladybird macOS sandbox default deny"))

(deny process-info*)
(allow process-info-pidinfo process-info-rusage process-info-setcontrol
    (target self))
(allow signal (target self))
(allow sysctl-read
    (sysctl-name
        "hw.activecpu"
        "hw.byteorder"
        "hw.cachelinesize"
        "hw.cachesize"
        "hw.cpufamily"
        "hw.cpusubfamily"
        "hw.cputype"
        "hw.l1dcachesize"
        "hw.l1icachesize"
        "hw.l2cachesize"
        "hw.l3cachesize"
        "hw.logicalcpu"
        "hw.logicalcpu_max"
        "hw.machine"
        "hw.memsize"
        "hw.model"
        "hw.ncpu"
        "hw.nperflevels"
        "hw.pagesize"
        "hw.pagesize_compat"
        "hw.physicalcpu"
        "hw.physicalcpu_max"
        "hw.tbfrequency"
        "hw.tbfrequency_compat"
        "hw.vectorunit"
        "kern.bootargs"
        "kern.hv_vmm_present"
        "kern.maxfilesperproc"
        "kern.osproductversion"
        "kern.osrelease"
        "kern.ostype"
        "kern.osvariant_status"
        "kern.osversion"
        "kern.secure_kernel"
        "kern.usrstack64"
        "kern.version"
        "kern.willshutdown"
        "machdep.cpu.brand_string"
        "sysctl.name2oid"
        "sysctl.proc_cputype"
        "sysctl.proc_translated"
        "vm.malloc_ranges")
    (sysctl-name-prefix "hw.optional.")
    (sysctl-name-prefix "hw.perflevel"))
(allow ipc-posix-shm-write-create ipc-posix-shm-write-unlink
    (ipc-posix-name-prefix "/shm-"))
(allow iokit-open-user-client
    (iokit-user-client-class "IOSurfaceRootUserClient"))

(allow network-outbound
    (literal "/private/var/run/syslog"))

(deny syscall-unix
    (with send-signal SIGKILL)
    (with message "Ladybird macOS sandbox syscall deny"))

(allow syscall-unix
    (syscall-group-bsdthread)
    (syscall-group-close)
    (syscall-group-fcntl)
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
        SYS___mac_syscall
        SYS___semwait_signal
        SYS___semwait_signal_nocancel
        SYS_abort_with_payload
        SYS_access
        SYS_change_fdguard_np
        SYS_connect
        SYS_crossarch_trap
        SYS_csops_audittoken
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
        SYS_getpeername
        SYS_getpid
        SYS_getpriority
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
        SYS_open
        SYS_open_nocancel
        SYS_openat
        SYS_os_fault_with_payload
        SYS_pathconf
        SYS_persona
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

; System frameworks ask for the mount table, the host UUID and the System Integrity Protection state while they set
; themselves up, and cope when they cannot have them. Fail these calls instead of killing the helper.
(deny syscall-unix
    (with errno 1)
    (syscall-number
        SYS_csrctl
        SYS_getfsstat
        SYS_getfsstat64
        SYS_gethostuuid))

(deny file-lock)

; NB: dyld needs F_ADDFILESIGS_RETURN to load libraries after the sandbox is in place, for example Metal's GPU plugin.
(deny system-fcntl)
(allow system-fcntl
    (fcntl-command
        F_ADDFILESIGS_RETURN
        F_BARRIERFSYNC
        F_CHECK_LV
        F_DUPFD
        F_DUPFD_CLOEXEC
        F_FULLFSYNC
        F_GETFD
        F_GETFL
        F_GETLK
        F_GETNOSIGPIPE
        F_GETPROTECTIONCLASS
        F_NOCACHE
        F_OFD_GETLK
        F_OFD_SETLK
        F_OFD_SETLKW
        F_RDADVISE
        F_SETFD
        F_SETFL
        F_SETLK
        F_SETLKW
        F_SETNOSIGPIPE))

(deny file-test-existence)

(allow file-read-metadata file-test-existence
    (literal "/Library")
    (literal "/System/Volumes/Data")
    (literal "/etc")
    (literal "/private")
    (literal "/private/etc")
    (literal "/private/tmp")
    (literal "/private/var")
    (literal "/private/var/db")
    (literal "/tmp")
    (literal "/usr")
    (literal "/var"))

(allow file-read* file-test-existence
    (literal "/")
    (literal "/dev/null")
    (literal "/dev/random")
    (literal "/dev/urandom")
    (literal "/etc/localtime")
    (literal "/private/etc/localtime")
    (subpath "/etc/ssl")
    (subpath "/private/etc/ssl")
    (subpath "/System")
    (subpath "/Library/Preferences/Logging")
    (subpath "/private/var/db/timezone")
    (subpath "/var/db/timezone")
    (subpath "/usr/lib")
    (subpath "/usr/share"))

(allow file-map-executable
    (subpath "/System")
    (subpath "/usr/lib"))

)~~~"sv));

    if (options.network_access == NetworkAccess::Allowed) {
        TRY(profile.try_append(R"~~~(
(allow network*)
(allow sysctl-read
    (sysctl-name-prefix "net.routetable."))
(allow system-socket
    (require-all
        (socket-domain AF_SYSTEM)
        (socket-protocol 2)))
(allow system-necp-client-action)
(allow file-test-existence
    (literal "/private/var/run/mDNSResponder")
    (literal "/var/run/mDNSResponder"))
(allow syscall-unix
    (syscall-number
        SYS___channel_open
        SYS_necp_client_action
        SYS_necp_open))
)~~~"sv));
    }

    TRY(append_allowed_paths(profile, "file-read* file-test-existence"sv, options.paths, SeatbeltPath::Access::ReadOnly));
    TRY(append_allowed_ancestor_directories(profile, options.paths));
    TRY(append_allowed_paths(profile, "file-map-executable"sv, options.paths, SeatbeltPath::Access::ReadAndExecute));
    TRY(append_allowed_paths(profile, "file-write*"sv, options.paths, SeatbeltPath::Access::ReadWrite));
    TRY(append_allowed_paths(profile, "file-lock"sv, options.paths, SeatbeltPath::Access::ReadWrite));
    TRY(append_allowed_path_extensions(profile, options.paths, SeatbeltPath::Access::ReadOnly));
    TRY(append_allowed_path_extensions(profile, options.paths, SeatbeltPath::Access::ReadWrite));
    TRY(append_allowed_executables(profile, options.executable_paths));
    TRY(append_allowed_iokit_user_client_classes(profile, options.iokit_user_client_classes));
    TRY(append_allowed_mach_services(profile, options));

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
