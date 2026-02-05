/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FileWatcher.h"
#include <LibCore/Notifier.h>
#include <errno.h>

namespace Core {

ErrorOr<NonnullRefPtr<FileWatcher>> FileWatcher::create(FileWatcherFlags)
{
    return Error::from_errno(ENOTSUP);
}

FileWatcher::FileWatcher(int watcher_fd, NonnullRefPtr<Notifier> notifier)
    : FileWatcherBase(watcher_fd)
    , m_notifier(move(notifier))
{
}

FileWatcher::~FileWatcher() = default;

ErrorOr<bool> FileWatcherBase::add_watch(ByteString, FileWatcherEvent::Type)
{
    return Error::from_errno(ENOTSUP);
}

ErrorOr<bool> FileWatcherBase::remove_watch(ByteString)
{
    return Error::from_errno(ENOTSUP);
}

}
