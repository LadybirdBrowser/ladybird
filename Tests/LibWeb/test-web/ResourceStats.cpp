/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ResourceStats.h"

#include <AK/Optional.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/DirIterator.h>
#include <LibCore/System.h>

#if defined(AK_OS_MACOS)
#    include <libproc.h>
#    include <sys/proc_info.h>
#endif

#if !defined(AK_OS_WINDOWS)
#    include <sys/resource.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace TestWeb {

static ResourceStats query_resource_stats();

ResourceStats resource_stats(bool force_update)
{
    static ResourceStats stats = query_resource_stats();
    static UnixDateTime last_update = UnixDateTime::now();

    if (force_update || (UnixDateTime::now() - last_update).to_truncated_milliseconds() >= 200) {
        stats = query_resource_stats();
        last_update = UnixDateTime::now();
    }

    return stats;
}

#if defined(AK_OS_MACOS)
ResourceStats query_resource_stats()
{
    ResourceStats stats;
    struct rlimit limits;
    if (getrlimit(RLIMIT_NOFILE, &limits) == 0)
        stats.fd_limit = limits.rlim_cur;

    pid_t const pid = Core::System::getpid();
    Vector<proc_fdinfo> fd_infos;

    for (size_t capacity = 256;; capacity *= 2) {
        fd_infos.resize(capacity);
        int const buffer_size = static_cast<int>(capacity * sizeof(proc_fdinfo));
        int const bytes_filled = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fd_infos.data(), buffer_size);
        if (bytes_filled <= 0) {
            fd_infos.clear();
            break;
        }

        if (bytes_filled < buffer_size) {
            fd_infos.resize(static_cast<size_t>(bytes_filled) / sizeof(proc_fdinfo));
            break;
        }
    }

    stats.open_fds = fd_infos.size();
    for (auto const& fd_info : fd_infos) {
        switch (fd_info.proc_fdtype) {
        case PROX_FDTYPE_VNODE:
            ++stats.files;
            break;
        case PROX_FDTYPE_SOCKET:
            ++stats.sockets;
            break;
        case PROX_FDTYPE_PIPE:
            ++stats.pipes;
            break;
        case PROX_FDTYPE_PSHM:
            ++stats.shared_memory;
            break;
        default:
            ++stats.other;
            break;
        }
    }
    return stats;
}

#elif defined(AK_OS_LINUX)

static bool is_shmem(StringView target)
{
    return target.starts_with("/dev/shm/"sv)
        || target.starts_with("/memfd:"sv)
        || target.starts_with("memfd:"sv);
}

static void classify_linux_fd(ResourceStats& stats, int fd, StringView target)
{
    struct stat stat_buffer;
    if (fstat(fd, &stat_buffer) == 0) {
        if (S_ISSOCK(stat_buffer.st_mode)) {
            ++stats.sockets;
            return;
        }
        if (S_ISFIFO(stat_buffer.st_mode)) {
            ++stats.pipes;
            return;
        }
        if (S_ISREG(stat_buffer.st_mode) || S_ISDIR(stat_buffer.st_mode) || S_ISCHR(stat_buffer.st_mode) || S_ISBLK(stat_buffer.st_mode)) {
            if (is_shmem(target)) {
                ++stats.shared_memory;
                return;
            }
            ++stats.files;
            return;
        }
    }
    if (target.starts_with("socket:"sv))
        ++stats.sockets;
    else if (target.starts_with("pipe:"sv))
        ++stats.pipes;
    else if (is_shmem(target))
        ++stats.shared_memory;
    else if (!target.is_empty())
        ++stats.files;
    else
        ++stats.other;
}
static ByteString get_link_target(ByteString const& path)
{
    Vector<char> buffer;
    buffer.resize(PATH_MAX);
    for (;;) {
        ssize_t const bytes_read = ::readlink(path.characters(), buffer.data(), buffer.size());
        if (bytes_read < 0)
            return {};
        if (static_cast<size_t>(bytes_read) < buffer.size())
            return ByteString(buffer.data(), static_cast<size_t>(bytes_read));

        buffer.resize(buffer.size() * 2);
    }
}

ResourceStats query_resource_stats()
{
    ResourceStats stats;
    struct rlimit limits;
    if (getrlimit(RLIMIT_NOFILE, &limits) == 0)
        stats.fd_limit = limits.rlim_cur;

    Core::DirIterator fd_iterator("/proc/self/fd"sv, Core::DirIterator::Flags::SkipDots);
    if (fd_iterator.has_error())
        return stats;

    int const iterator_fd = fd_iterator.fd();
    while (fd_iterator.has_next()) {
        Optional<Core::DirectoryEntry> entry = fd_iterator.next();
        if (!entry.has_value())
            break;

        Optional<int> maybe_fd = entry->name.to_number<int>();
        if (!maybe_fd.has_value())
            continue;

        int const fd = maybe_fd.value();
        if (fd == iterator_fd)
            continue;

        ++stats.open_fds;
        ByteString target = get_link_target(ByteString::formatted("/proc/self/fd/{}", fd));
        classify_linux_fd(stats, fd, target);
    }

    return stats;
}

#else

ResourceStats query_resource_stats()
{
    ResourceStats stats;
    return stats;
}
#endif

}
