/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/HashTable.h>
#include <AK/Noncopyable.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibSandbox/FileBroker.h>
#include <LibSandbox/Seccomp.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Sandbox {

using namespace Detail;

static int s_file_broker_socket = -1;

int Detail::file_broker_socket()
{
    return s_file_broker_socket;
}

bool file_broker_is_active()
{
    return s_file_broker_socket >= 0;
}

static bool is_same_or_beneath(StringView path, StringView ancestor)
{
    if (path == ancestor || ancestor == "/"sv)
        return true;
    return path.length() > ancestor.length() && path.starts_with(ancestor) && path[ancestor.length()] == '/';
}

bool file_broker_allows(ReadonlySpan<ResolvedPathRule> rules, StringView canonical_path, FileBrokerAccess access)
{
    for (auto const& rule : rules) {
        StringView rule_path = rule.resolved_path;

        if (access == FileBrokerAccess::ChangeEntry) {
            if (rule.is_directory && rule.access == PathRule::Access::ReadWrite && canonical_path != rule_path && is_same_or_beneath(canonical_path, rule_path))
                return true;
            continue;
        }

        auto covered = rule.is_directory ? is_same_or_beneath(canonical_path, rule_path) : canonical_path == rule_path;
        if (covered) {
            if (access != FileBrokerAccess::Write || rule.access == PathRule::Access::ReadWrite)
                return true;
        }

        if (access == FileBrokerAccess::Metadata && (is_same_or_beneath(rule_path, canonical_path) || is_same_or_beneath(rule.requested_path, canonical_path)))
            return true;
    }
    return false;
}

namespace {

enum class WalkMode {
    FollowLast,
    StayOnLast,
    StopBeforeLast,
};

// Walks a path one component at a time without ever following a symbolic link inside the kernel. The helper can change
// the files in a directory it may write to, so a path checked first and opened afterwards could lead somewhere else by
// the time it is opened. Here, the descriptor that is checked is the one that gets used.
class PathWalker {
    AK_MAKE_NONCOPYABLE(PathWalker);

public:
    explicit PathWalker(pid_t helper_pid)
        : m_helper_pid(helper_pid)
    {
    }

    ~PathWalker()
    {
        for (auto fd : m_directories)
            close(fd);
    }

    int walk(StringView absolute_path, WalkMode mode)
    {
        // A trailing slash asks for what the link leads to, even from a system call that otherwise stays on the link.
        if (mode == WalkMode::StayOnLast && absolute_path.ends_with('/'))
            mode = WalkMode::FollowLast;

        auto root = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (root < 0)
            return errno;
        m_directories.append(root);
        if (fstat(root, &m_stat) < 0)
            return errno;

        Vector<ByteString> pending;
        push_components(pending, absolute_path);

        size_t followed_links = 0;
        while (!pending.is_empty()) {
            auto name = pending.take_last();
            if (name == ".."sv) {
                if (!m_names.is_empty()) {
                    close(m_directories.take_last());
                    m_names.take_last();
                    if (auto error = refresh_stat(); error != 0)
                        return error;
                }
                continue;
            }

            auto is_last = pending.is_empty();
            if (is_last && mode == WalkMode::StopBeforeLast) {
                m_last_name = name;
                return 0;
            }

            // The broker is a process of its own, so what the helper calls /proc/self has to be spelled out.
            if (is_directly_in_proc() && name == "self"sv) {
                name = ByteString::number(m_helper_pid);
                m_ended_on_proc_self = is_last;
            } else if (is_directly_in_proc() && name == "thread-self"sv) {
                return fail_at(name, ENOENT);
            }

            auto fd = openat(m_directories.last(), name.characters(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0)
                return fail_at(name, errno);

            struct stat statbuf;
            if (fstat(fd, &statbuf) < 0) {
                auto error = errno;
                close(fd);
                return fail_at(name, error);
            }

            if (S_ISLNK(statbuf.st_mode) && !(is_last && mode == WalkMode::StayOnLast)) {
                char target[PATH_MAX];
                auto length = readlinkat(fd, "", target, sizeof(target));
                auto error = errno;
                close(fd);
                if (length < 0)
                    return fail_at(name, error);
                if (static_cast<size_t>(length) == sizeof(target))
                    return fail_at(name, ENAMETOOLONG);
                if (++followed_links > 40)
                    return fail_at(name, ELOOP);

                StringView target_view { target, static_cast<size_t>(length) };
                if (target_view.starts_with('/')) {
                    while (!m_names.is_empty()) {
                        close(m_directories.take_last());
                        m_names.take_last();
                    }
                    if (auto error = refresh_stat(); error != 0)
                        return error;
                }
                push_components(pending, target_view);
                continue;
            }

            m_directories.append(fd);
            m_names.append(move(name));
            m_stat = statbuf;
        }

        // There was nothing before the last component to stop at, as in a request for the root directory itself.
        if (mode == WalkMode::StopBeforeLast)
            return EEXIST;

        // A trailing slash only ever names a directory.
        if (absolute_path.ends_with('/') && !S_ISDIR(m_stat.st_mode)) {
            m_failed_path = canonical_path();
            return ENOTDIR;
        }
        return 0;
    }

    ByteString canonical_path() const
    {
        if (m_names.is_empty())
            return "/"sv;
        StringBuilder builder;
        for (auto const& name : m_names) {
            builder.append('/');
            builder.append(name);
        }
        return builder.to_byte_string();
    }

    ByteString canonical_path_with_last_name() const { return join(canonical_path(), m_last_name); }
    ByteString const& failed_path() const { return m_failed_path; }
    ByteString const& last_name() const { return m_last_name; }
    bool ended_on_proc_self() const { return m_ended_on_proc_self; }
    int fd() const { return m_directories.last(); }
    struct stat const& stat_result() const { return m_stat; }

private:
    static ByteString join(StringView directory, StringView name)
    {
        if (directory == "/"sv)
            return ByteString::formatted("/{}", name);
        return ByteString::formatted("{}/{}", directory, name);
    }

    static void push_components(Vector<ByteString>& pending, StringView path)
    {
        auto parts = path.split_view('/');
        for (auto const& part : parts.in_reverse()) {
            if (part != "."sv)
                pending.append(part);
        }
    }

    bool is_directly_in_proc() const { return m_names.size() == 1 && m_names.first() == "proc"sv; }

    // Leaving a directory by any way other than a new component has to report the directory that we ended up in.
    int refresh_stat()
    {
        return fstat(m_directories.last(), &m_stat) < 0 ? errno : 0;
    }

    int fail_at(StringView name, int error)
    {
        m_failed_path = join(canonical_path(), name);
        return error;
    }

    pid_t m_helper_pid { 0 };
    Vector<int> m_directories;
    Vector<ByteString> m_names;
    ByteString m_last_name;
    ByteString m_failed_path;
    bool m_ended_on_proc_self { false };
    struct stat m_stat {};
};

class FileBrokerServer {
public:
    FileBrokerServer(Vector<ResolvedPathRule> rules, pid_t helper_pid, ByteString current_directory, ByteString helper_name)
        : m_rules(move(rules))
        , m_helper_pid(helper_pid)
        , m_current_directory(move(current_directory))
        , m_helper_name(move(helper_name))
        , m_logs_refusals(sandbox_debugging_is_enabled())
    {
    }

    void serve(int socket);

private:
    struct Result {
        int error { 0 };
        int fd { -1 };
    };

    bool allows(StringView path, FileBrokerAccess access) const
    {
        if (file_broker_allows(m_rules, path, access))
            return true;
        log_refusal(path, access);
        return false;
    }

    static constexpr size_t max_logged_refusals = 4096;

    // Each path is logged once for each kind of access, since libraries tend to ask for the same files again and again.
    void log_refusal(StringView path, FileBrokerAccess access) const
    {
        if (!m_logs_refusals)
            return;
        if (m_logged_too_many_refusals)
            return;
        auto key = ByteString::formatted("{}:{}", to_underlying(access), path);
        if (m_logged_refusals.contains(key))
            return;
        if (m_logged_refusals.size() == max_logged_refusals) {
            m_logged_too_many_refusals = true;
            warnln("Sandbox refusal in {}: too many refusals, further refusals are not logged", m_helper_name);
            return;
        }
        m_logged_refusals.set(key);
        auto access_name = [&] {
            switch (access) {
            case FileBrokerAccess::Metadata:
                return "metadata"sv;
            case FileBrokerAccess::Read:
                return "read"sv;
            case FileBrokerAccess::Write:
                return "write"sv;
            case FileBrokerAccess::ChangeEntry:
                return "entry change"sv;
            }
            VERIFY_NOT_REACHED();
        }();
        warnln("Sandbox refusal in {}: {} access to {}", m_helper_name, access_name, path);
    }

    // A path outside of what the helper can see has to look the same whether or not it exists. Hiding that a path is
    // missing changes nothing for the helper, so only hiding one that exists is logged.
    int failure_for(PathWalker const& walker, int error) const
    {
        if (file_broker_allows(m_rules, walker.failed_path(), FileBrokerAccess::Metadata))
            return error;
        if (error != ENOENT)
            log_refusal(walker.failed_path(), FileBrokerAccess::Metadata);
        return EACCES;
    }

    ErrorOr<ByteString> absolute_path(StringView path, int directory_fd) const;

    Result open_file(StringView path, int flags, u32 mode);
    Result reopen(PathWalker const&, int flags);
    int access_file(StringView path, int mode, u32 at_flags);
    int stat_file(StringView path, bool follow, struct stat& result);
    int read_link(StringView path, char* buffer, u32& length);
    int make_directory(StringView path, u32 mode);
    int remove(StringView path, bool directory);
    int rename_file(StringView old_path, StringView new_path);
    int bind_socket(int fd, char const* address, u32 length);

    void handle(FileBrokerRequest const&, int directory_fds[2], int reply_fd);

    Vector<ResolvedPathRule> m_rules;
    pid_t m_helper_pid { 0 };
    ByteString m_current_directory;
    ByteString m_helper_name;
    bool m_logs_refusals { false };
    mutable HashTable<ByteString> m_logged_refusals;
    mutable bool m_logged_too_many_refusals { false };
};

}

// Relative paths start from the directory that the helper named with a descriptor, or else from its working directory.
ErrorOr<ByteString> FileBrokerServer::absolute_path(StringView path, int directory_fd) const
{
    if (path.is_empty())
        return Error::from_errno(ENOENT);
    if (path.starts_with('/'))
        return ByteString { path };
    if (directory_fd < 0) {
        if (m_current_directory.is_empty())
            return Error::from_errno(ENOENT);
        return ByteString::formatted("{}/{}", m_current_directory, path);
    }

    char link[32];
    (void)snprintf(link, sizeof(link), "/proc/self/fd/%d", directory_fd);
    char directory[PATH_MAX];
    auto length = readlink(link, directory, sizeof(directory));
    if (length < 0)
        return Error::from_errno(errno);
    if (static_cast<size_t>(length) == sizeof(directory))
        return Error::from_errno(ENAMETOOLONG);
    // A socket or a pipe has a name too, but no file system path to start from.
    if (length == 0 || directory[0] != '/')
        return Error::from_errno(ENOTDIR);
    return ByteString::formatted("{}/{}", StringView { directory, static_cast<size_t>(length) }, path);
}

// Goes through the descriptor of the file that was checked, so the open cannot land on any other file.
FileBrokerServer::Result FileBrokerServer::reopen(PathWalker const& walker, int flags)
{
    char link[32];
    (void)snprintf(link, sizeof(link), "/proc/self/fd/%d", walker.fd());
    auto fd = open(link, (flags & ~(O_CREAT | O_EXCL | O_NOFOLLOW)) | O_CLOEXEC);
    if (fd < 0)
        return { errno, -1 };
    return { 0, fd };
}

FileBrokerServer::Result FileBrokerServer::open_file(StringView path, int flags, u32 mode)
{
    // A nameless temporary file needs a directory that the rules say nothing about, and callers can do without one.
    if ((flags & O_TMPFILE) == O_TMPFILE)
        return { EOPNOTSUPP, -1 };

    auto access_mode = flags & O_ACCMODE;
    // O_CREAT writes nothing to a file that already exists, and the path that makes a new one has its own check.
    auto wants_write = access_mode != O_RDONLY || (flags & O_TRUNC) != 0;
    auto needed = wants_write ? FileBrokerAccess::Write : FileBrokerAccess::Read;

    auto creates_exclusively = (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
    auto stay_on_last = (flags & O_NOFOLLOW) != 0 || creates_exclusively;
    {
        PathWalker walker { m_helper_pid };
        auto error = walker.walk(path, stay_on_last ? WalkMode::StayOnLast : WalkMode::FollowLast);
        if (error == 0) {
            if ((flags & O_PATH) != 0 ? !allows(walker.canonical_path(), FileBrokerAccess::Metadata) : !allows(walker.canonical_path(), needed))
                return { EACCES, -1 };
            if (creates_exclusively)
                return { EEXIST, -1 };
            if (S_ISLNK(walker.stat_result().st_mode) && (flags & O_PATH) == 0)
                return { ELOOP, -1 };
            return reopen(walker, flags);
        }
        if (error != ENOENT || (flags & O_CREAT) == 0)
            return { failure_for(walker, error), -1 };
    }

    if (path.ends_with('/'))
        return { EISDIR, -1 };

    PathWalker walker { m_helper_pid };
    if (auto error = walker.walk(path, WalkMode::StopBeforeLast); error != 0)
        return { failure_for(walker, error), -1 };
    if (!allows(walker.canonical_path_with_last_name(), FileBrokerAccess::ChangeEntry))
        return { EACCES, -1 };

    auto fd = openat(walker.fd(), walker.last_name().characters(), flags | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0)
        return { errno, -1 };
    return { 0, fd };
}

int FileBrokerServer::access_file(StringView path, int mode, u32 at_flags)
{
    PathWalker walker { m_helper_pid };
    if (auto error = walker.walk(path, (at_flags & AT_SYMLINK_NOFOLLOW) ? WalkMode::StayOnLast : WalkMode::FollowLast); error != 0)
        return failure_for(walker, error);

    auto needed = FileBrokerAccess::Metadata;
    if (mode & W_OK)
        needed = FileBrokerAccess::Write;
    else if (mode & (R_OK | X_OK))
        needed = FileBrokerAccess::Read;
    if (!allows(walker.canonical_path(), needed))
        return EACCES;

    char link[32];
    (void)snprintf(link, sizeof(link), "/proc/self/fd/%d", walker.fd());
    return access(link, mode) < 0 ? errno : 0;
}

int FileBrokerServer::stat_file(StringView path, bool follow, struct stat& result)
{
    PathWalker walker { m_helper_pid };
    if (auto error = walker.walk(path, follow ? WalkMode::FollowLast : WalkMode::StayOnLast); error != 0)
        return failure_for(walker, error);
    if (!allows(walker.canonical_path(), FileBrokerAccess::Metadata))
        return EACCES;
    result = walker.stat_result();
    return 0;
}

int FileBrokerServer::read_link(StringView path, char* buffer, u32& length)
{
    PathWalker walker { m_helper_pid };
    if (auto error = walker.walk(path, WalkMode::StayOnLast); error != 0)
        return failure_for(walker, error);
    if (!allows(walker.canonical_path(), FileBrokerAccess::Metadata))
        return EACCES;

    // The walk replaced this link with the helper's own directory, but the link is what the helper asked about.
    if (walker.ended_on_proc_self()) {
        auto helper_pid = ByteString::number(m_helper_pid);
        memcpy(buffer, helper_pid.characters(), helper_pid.length());
        length = static_cast<u32>(helper_pid.length());
        return 0;
    }

    if (!S_ISLNK(walker.stat_result().st_mode))
        return EINVAL;

    auto result = readlinkat(walker.fd(), "", buffer, PATH_MAX);
    if (result < 0)
        return errno;
    length = static_cast<u32>(result);
    return 0;
}

int FileBrokerServer::make_directory(StringView path, u32 mode)
{
    PathWalker walker { m_helper_pid };
    if (auto error = walker.walk(path, WalkMode::StopBeforeLast); error != 0)
        return failure_for(walker, error);

    // The kernel reports a directory that exists before it asks whether a new one could be made there, and libpulse
    // depends on that for a runtime directory that it may only read.
    auto target = walker.canonical_path_with_last_name();
    if (!file_broker_allows(m_rules, target, FileBrokerAccess::ChangeEntry)) {
        struct stat statbuf;
        if (file_broker_allows(m_rules, target, FileBrokerAccess::Metadata) && fstatat(walker.fd(), walker.last_name().characters(), &statbuf, AT_SYMLINK_NOFOLLOW) == 0)
            return EEXIST;
        log_refusal(target, FileBrokerAccess::ChangeEntry);
        return EACCES;
    }
    return mkdirat(walker.fd(), walker.last_name().characters(), mode) < 0 ? errno : 0;
}

int FileBrokerServer::remove(StringView path, bool directory)
{
    PathWalker walker { m_helper_pid };
    if (auto error = walker.walk(path, WalkMode::StopBeforeLast); error != 0)
        return failure_for(walker, error);
    if (!allows(walker.canonical_path_with_last_name(), FileBrokerAccess::ChangeEntry))
        return EACCES;
    return unlinkat(walker.fd(), walker.last_name().characters(), directory ? AT_REMOVEDIR : 0) < 0 ? errno : 0;
}

int FileBrokerServer::rename_file(StringView old_path, StringView new_path)
{
    PathWalker old_walker { m_helper_pid };
    if (auto error = old_walker.walk(old_path, WalkMode::StopBeforeLast); error != 0)
        return failure_for(old_walker, error);
    PathWalker new_walker { m_helper_pid };
    if (auto error = new_walker.walk(new_path, WalkMode::StopBeforeLast); error != 0)
        return failure_for(new_walker, error);

    if (!allows(old_walker.canonical_path_with_last_name(), FileBrokerAccess::ChangeEntry) || !allows(new_walker.canonical_path_with_last_name(), FileBrokerAccess::ChangeEntry))
        return EACCES;
    return renameat(old_walker.fd(), old_walker.last_name().characters(), new_walker.fd(), new_walker.last_name().characters()) < 0 ? errno : 0;
}

// Binding a UNIX socket to a path creates a file that no path rule covers, so only the families that the network policies
// exist for get through. The kernel refuses an address whose family differs from the socket's own, and the address was
// copied out of the helper before this check, so the helper can't change it afterwards.
int FileBrokerServer::bind_socket(int fd, char const* address, u32 length)
{
    sockaddr_storage storage;
    if (fd < 0 || length < sizeof(sa_family_t) || length > sizeof(storage))
        return EINVAL;
    memcpy(&storage, address, length);
    if (storage.ss_family != AF_INET && storage.ss_family != AF_INET6 && storage.ss_family != AF_NETLINK) {
        if (m_logs_refusals)
            warnln("Sandbox refusal in {}: binding a socket of address family {}", m_helper_name, storage.ss_family);
        return EACCES;
    }
    return bind(fd, reinterpret_cast<sockaddr const*>(&storage), length) < 0 ? errno : 0;
}

static void send_response(int reply_fd, FileBrokerResponse const& response, int fd)
{
    iovec io {
        .iov_base = const_cast<FileBrokerResponse*>(&response),
        .iov_len = sizeof(response),
    };

    union {
        cmsghdr header;
        char space[CMSG_SPACE(sizeof(int))];
    } control {};

    msghdr message {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = &io,
        .msg_iovlen = 1,
        .msg_control = nullptr,
        .msg_controllen = 0,
        .msg_flags = 0,
    };

    if (fd >= 0) {
        message.msg_control = &control;
        message.msg_controllen = sizeof(control);
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    }

    // Never wait on a socket that the helper chose. If its buffer is full, the helper did that to itself.
    ssize_t sent = 0;
    do {
        sent = sendmsg(reply_fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (sent < 0 && errno == EINTR);
}

void FileBrokerServer::handle(FileBrokerRequest const& request, int directory_fds[2], int reply_fd)
{
    FileBrokerResponse response;
    memset(&response, 0, sizeof(response));
    response.magic = file_broker_magic;

    int result_fd = -1;
    ScopeGuard send_and_close = [&] {
        send_response(reply_fd, response, result_fd);
        if (result_fd >= 0)
            close(result_fd);
    };

    if (static_cast<FileBrokerOperation>(request.operation) == FileBrokerOperation::BindSocket) {
        response.error = bind_socket(directory_fds[0], request.path, request.path_length);
        return;
    }

    StringView raw_path { request.path, request.path_length };
    StringView raw_second_path { request.second_path, request.second_path_length };
    if (raw_path.contains('\0') || raw_second_path.contains('\0')) {
        response.error = EINVAL;
        return;
    }

    auto path_or_error = absolute_path(raw_path, directory_fds[0]);
    if (path_or_error.is_error()) {
        response.error = path_or_error.error().code();
        return;
    }
    auto path = path_or_error.release_value();

    switch (static_cast<FileBrokerOperation>(request.operation)) {
    case FileBrokerOperation::Open: {
        auto result = open_file(path, request.flags, request.mode);
        response.error = result.error;
        result_fd = result.fd;
        return;
    }
    case FileBrokerOperation::Access:
        response.error = access_file(path, request.flags, request.mode);
        return;
    case FileBrokerOperation::Stat:
        response.error = stat_file(path, true, response.stat_result);
        return;
    case FileBrokerOperation::SymlinkStat:
        response.error = stat_file(path, false, response.stat_result);
        return;
    case FileBrokerOperation::ReadLink:
        response.error = read_link(path, response.link, response.link_length);
        return;
    case FileBrokerOperation::MakeDirectory:
        response.error = make_directory(path, request.mode);
        return;
    case FileBrokerOperation::RemoveFile:
        response.error = remove(path, false);
        return;
    case FileBrokerOperation::RemoveDirectory:
        response.error = remove(path, true);
        return;
    case FileBrokerOperation::BindSocket:
        VERIFY_NOT_REACHED();
    case FileBrokerOperation::Rename: {
        auto second_or_error = absolute_path(raw_second_path, directory_fds[1]);
        if (second_or_error.is_error()) {
            response.error = second_or_error.error().code();
            return;
        }
        response.error = rename_file(path, second_or_error.value());
        return;
    }
    }
    response.error = EINVAL;
}

void FileBrokerServer::serve(int socket)
{
    static FileBrokerRequest request;

    for (;;) {
        iovec io {
            .iov_base = &request,
            .iov_len = sizeof(request),
        };

        // Room for more than a well-behaved helper sends, so that anything extra can be counted and closed.
        union {
            cmsghdr header;
            char space[CMSG_SPACE(4 * sizeof(int))];
        } control {};

        msghdr message {
            .msg_name = nullptr,
            .msg_namelen = 0,
            .msg_iov = &io,
            .msg_iovlen = 1,
            .msg_control = &control,
            .msg_controllen = sizeof(control),
            .msg_flags = 0,
        };

        ssize_t received = 0;
        do {
            received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
        } while (received < 0 && errno == EINTR);
        if (received < 0)
            return;

        int fds[8];
        size_t fd_count = 0;
        for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS || header->cmsg_len < CMSG_LEN(0))
                continue;
            auto count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count && fd_count < 8; ++i)
                memcpy(&fds[fd_count++], CMSG_DATA(header) + i * sizeof(int), sizeof(int));
        }
        ScopeGuard close_fds = [&] {
            for (size_t i = 0; i < fd_count; ++i)
                close(fds[i]);
        };

        if (received == 0 && fd_count == 0)
            return;

        size_t expected_fd_count = 1;
        int directory_fds[2] = { -1, -1 };
        if (static_cast<size_t>(received) != sizeof(request)
            || request.magic != file_broker_magic
            || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0
            || request.path_length >= sizeof(request.path)
            || request.second_path_length >= sizeof(request.second_path)
            || (request.directory_fd_mask & ~3u) != 0) {
            continue;
        }

        if (request.directory_fd_mask & 1)
            ++expected_fd_count;
        if (request.directory_fd_mask & 2)
            ++expected_fd_count;
        if (fd_count != expected_fd_count)
            continue;

        size_t next_fd = 1;
        if (request.directory_fd_mask & 1)
            directory_fds[0] = fds[next_fd++];
        if (request.directory_fd_mask & 2)
            directory_fds[1] = fds[next_fd++];

        handle(request, directory_fds, fds[0]);
    }
}

static void close_all_descriptors_except(int keep)
{
    auto* directory = opendir("/proc/self/fd");
    if (!directory)
        _exit(1);

    Vector<int> to_close;
    while (auto* entry = readdir(directory)) {
        auto fd = atoi(entry->d_name);
        if (entry->d_name[0] != '.' && fd > STDERR_FILENO && fd != keep && fd != dirfd(directory))
            to_close.append(fd);
    }
    closedir(directory);

    for (auto fd : to_close)
        close(fd);
}

static ErrorOr<void> confine_broker()
{
    SeccompPolicy policy;
    policy.allow_filesystem_metadata_queries();
    policy.allow_filesystem_writes();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_binding_sockets();
    policy.allow_memory_without_executable_mappings();
    policy.allow_signals();
    policy.allow_clocks();
    policy.allow_exit();
    return policy.install();
}

ErrorOr<void> start_file_broker(ReadonlySpan<PathRule> rules)
{
    Vector<ResolvedPathRule> resolved_rules;
    for (auto const& rule : rules) {
        char resolved_path[PATH_MAX];
        if (realpath(rule.path.characters(), resolved_path) == nullptr)
            return Error::from_syscall("realpath"sv, errno);
        TRY(resolved_rules.try_append({ rule.path, resolved_path, rule.access, rule.is_directory }));
    }

    // A working directory that was deleted only costs the helper its relative paths, so it must not cost it the sandbox.
    char current_directory[PATH_MAX];
    if (!getcwd(current_directory, sizeof(current_directory)))
        current_directory[0] = '\0';

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) < 0)
        return Error::from_syscall("socketpair"sv, errno);

    auto helper_pid = getpid();
    auto child = fork();
    if (child < 0) {
        auto error = errno;
        close(fds[0]);
        close(fds[1]);
        return Error::from_syscall("fork"sv, error);
    }

    if (child == 0) {
        close(fds[0]);

        // Everything else the helper has open is more than a broker for its files should ever hold.
        close_all_descriptors_except(fds[1]);

        char helper_name[16] = {};
        (void)prctl(PR_GET_NAME, helper_name, 0ul, 0ul, 0ul);
        (void)prctl(PR_SET_NAME, "FileBroker", 0ul, 0ul, 0ul);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0ul, 0ul, 0ul) < 0 || getppid() != helper_pid)
            _exit(1);

        FileBrokerServer server { move(resolved_rules), helper_pid, current_directory, ByteString { helper_name, strnlen(helper_name, sizeof(helper_name)) } };
        if (confine_broker().is_error())
            _exit(1);

        char const ready = 1;
        if (send(fds[1], &ready, 1, MSG_NOSIGNAL) != 1)
            _exit(1);

        server.serve(fds[1]);
        _exit(0);
    }

    close(fds[1]);

    // The helper stays honest about whether it is confined: without this, a broker that failed to start would look like
    // one that refuses every path.
    char ready = 0;
    ssize_t received = 0;
    do {
        received = recv(fds[0], &ready, 1, 0);
    } while (received < 0 && errno == EINTR);
    if (received != 1 || ready != 1) {
        close(fds[0]);
        return Error::from_string_literal("The file broker could not start");
    }

    s_file_broker_socket = fds[0];

    // Landlock confines the helper from here on, while it still sets up drivers and libraries. So must the broker.
    TRY(Detail::install_file_system_filter());
    return {};
}

}
