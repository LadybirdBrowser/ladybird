/*
 * Copyright (c) 2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/File.h>

#ifdef AK_OS_WINDOWS
#    include <AK/windows.h>
#endif

namespace IPC {

// FIXME: IPC::Files transferred over the wire are always set O_CLOEXEC during decoding.
//        Perhaps we should add an option to IPC::File to allow the receiver to decide whether to
//        make it O_CLOEXEC or not. Or an attribute in the .ipc file?
ErrorOr<void> File::clear_close_on_exec()
{
#ifndef AK_OS_WINDOWS
    auto fd_flags = TRY(Core::System::fcntl(m_fd, F_GETFD));
    fd_flags &= ~FD_CLOEXEC;
    TRY(Core::System::fcntl(m_fd, F_SETFD, fd_flags));
    return {};
#else
    HANDLE handle = (HANDLE)_get_osfhandle(m_fd);
    auto rc = SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (!rc)
        return Error::from_windows_error(GetLastError());
    return {};
#endif
}

}
