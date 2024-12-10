/*
 * Copyright (c) 2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/File.h>

namespace IPC {

// FIXME: IPC::Files transferred over the wire are always set O_CLOEXEC during decoding.
//        Perhaps we should add an option to IPC::File to allow the receiver to decide whether to
//        make it O_CLOEXEC or not. Or an attribute in the .ipc file?
ErrorOr<void> File::clear_close_on_exec()
{
    auto fd_flags = TRY(Core::System::fcntl(m_fd, F_GETFD));
    fd_flags &= ~FD_CLOEXEC;
    TRY(Core::System::fcntl(m_fd, F_SETFD, fd_flags));
    return {};
}

template<>
ErrorOr<File> decode(Decoder& decoder)
{
    auto file = TRY(decoder.files().try_dequeue());
    auto fd = file.fd();

    auto fd_flags = TRY(Core::System::fcntl(fd, F_GETFD));
    TRY(Core::System::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC));
    return file;
}

}
