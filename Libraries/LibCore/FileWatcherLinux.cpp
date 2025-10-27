/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FileWatcher.h"
#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/LexicalPath.h>
#include <LibCore/Notifier.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if !defined(AK_OS_LINUX)
static_assert(false, "This file must only be used for Linux");
#endif

namespace Core {

static constexpr unsigned file_watcher_flags_to_inotify_flags(FileWatcherFlags flags)
{
    unsigned result = 0;

    if (has_flag(flags, FileWatcherFlags::Nonblock))
        result |= IN_NONBLOCK;
    if (has_flag(flags, FileWatcherFlags::CloseOnExec))
        result |= IN_CLOEXEC;

    return result;
}

static Optional<FileWatcherEvent> get_event_from_fd(int fd, HashMap<unsigned, ByteString> const& wd_to_path)
{
    static constexpr auto max_event_size = sizeof(inotify_event) + NAME_MAX + 1;

    // Note from INOTIFY(7) man page:
    //
    //     Some systems cannot read integer variables if they are not properly aligned. On other
    //     systems, incorrect alignment may decrease performance. Hence, the buffer used for reading
    //     from the inotify file descriptor should have the same alignment as inotify_event.
    alignas(alignof(inotify_event)) Array<u8, max_event_size> buffer;
    ssize_t rc = ::read(fd, buffer.data(), buffer.size());

    if (rc == 0) {
        return {};
    } else if (rc < 0) {
        dbgln_if(FILE_WATCHER_DEBUG, "get_event_from_fd: Reading from wd {} failed: {}", fd, strerror(errno));
        return {};
    }

    auto const* event = reinterpret_cast<inotify_event const*>(buffer.data());
    FileWatcherEvent result;

    auto it = wd_to_path.find(event->wd);
    if (it == wd_to_path.end()) {
        dbgln_if(FILE_WATCHER_DEBUG, "get_event_from_fd: Got an event for a non-existent wd {}?!", event->wd);
        return {};
    }

    auto const& path = it->value;

    if ((event->mask & IN_CREATE) != 0)
        result.type |= FileWatcherEvent::Type::ChildCreated;
    if ((event->mask & IN_DELETE) != 0)
        result.type |= FileWatcherEvent::Type::ChildDeleted;
    if ((event->mask & IN_DELETE_SELF) != 0)
        result.type |= FileWatcherEvent::Type::Deleted;
    if ((event->mask & IN_MODIFY) != 0)
        result.type |= FileWatcherEvent::Type::ContentModified;
    if ((event->mask & IN_ATTRIB) != 0)
        result.type |= FileWatcherEvent::Type::MetadataModified;
    if ((event->mask & IN_IGNORED) != 0)
        return {};

    if (result.type == FileWatcherEvent::Type::Invalid) {
        warnln("Unknown event type {:x} returned by the watch_file descriptor for {}", event->mask, path);
        return {};
    }

    if (event->len > 0) {
        StringView child_name { event->name, strlen(event->name) };
        result.event_path = LexicalPath::join(path, child_name).string();
    } else {
        result.event_path = path;
    }

    dbgln_if(FILE_WATCHER_DEBUG, "get_event_from_fd: got event from wd {} on '{}' type {}", fd, result.event_path, result.type);
    return result;
}

ErrorOr<NonnullRefPtr<FileWatcher>> FileWatcher::create(FileWatcherFlags flags)
{
    auto watcher_fd = ::inotify_init1(file_watcher_flags_to_inotify_flags(flags | FileWatcherFlags::CloseOnExec));
    if (watcher_fd < 0)
        return Error::from_errno(errno);

    auto notifier = Notifier::construct(watcher_fd, Notifier::Type::Read);
    return adopt_nonnull_ref_or_enomem(new (nothrow) FileWatcher(watcher_fd, move(notifier)));
}

FileWatcher::FileWatcher(int watcher_fd, NonnullRefPtr<Notifier> notifier)
    : FileWatcherBase(watcher_fd)
    , m_notifier(move(notifier))
{
    m_notifier->on_activation = [this] {
        auto maybe_event = get_event_from_fd(m_notifier->fd(), m_wd_to_path);
        if (maybe_event.has_value()) {
            auto event = maybe_event.value();

            if (has_flag(event.type, FileWatcherEvent::Type::Deleted)) {
                auto result = remove_watch(event.event_path);
                if (result.is_error()) {
                    dbgln_if(FILE_WATCHER_DEBUG, "on_ready_to_read: {}", result.error());
                }
            }

            on_change(event);
        }
    };
}

FileWatcher::~FileWatcher() = default;

ErrorOr<bool> FileWatcherBase::add_watch(ByteString const& path, FileWatcherEvent::Type event_mask)
{
    if (m_path_to_wd.find(path) != m_path_to_wd.end()) {
        dbgln_if(FILE_WATCHER_DEBUG, "add_watch: path '{}' is already being watched", path);
        return false;
    }

    unsigned inotify_mask = 0;

    if (has_flag(event_mask, FileWatcherEvent::Type::ChildCreated))
        inotify_mask |= IN_CREATE;
    if (has_flag(event_mask, FileWatcherEvent::Type::ChildDeleted))
        inotify_mask |= IN_DELETE;
    if (has_flag(event_mask, FileWatcherEvent::Type::Deleted))
        inotify_mask |= IN_DELETE_SELF;
    if (has_flag(event_mask, FileWatcherEvent::Type::ContentModified))
        inotify_mask |= IN_MODIFY;
    if (has_flag(event_mask, FileWatcherEvent::Type::MetadataModified))
        inotify_mask |= IN_ATTRIB;
    if (has_flag(event_mask, FileWatcherEvent::Type::DoNotFollowLink))
        inotify_mask |= IN_DONT_FOLLOW;

    int watch_descriptor = ::inotify_add_watch(m_watcher_fd, path.characters(), inotify_mask);
    if (watch_descriptor < 0)
        return Error::from_errno(errno);

    m_path_to_wd.set(path, watch_descriptor);
    m_wd_to_path.set(watch_descriptor, path);

    dbgln_if(FILE_WATCHER_DEBUG, "add_watch: watching path '{}' on InodeWatcher {} wd {}", path, m_watcher_fd, watch_descriptor);
    return true;
}

ErrorOr<bool> FileWatcherBase::remove_watch(ByteString const& path)
{
    auto it = m_path_to_wd.find(path);
    if (it == m_path_to_wd.end()) {
        dbgln_if(FILE_WATCHER_DEBUG, "remove_watch: path '{}' is not being watched", path);
        return false;
    }

    m_path_to_wd.remove(it);
    m_wd_to_path.remove(it->value);

    if (::inotify_rm_watch(m_watcher_fd, it->value) < 0)
        return Error::from_errno(errno);

    dbgln_if(FILE_WATCHER_DEBUG, "remove_watch: stopped watching path '{}' on InodeWatcher {}", path, m_watcher_fd);
    return true;
}

}
