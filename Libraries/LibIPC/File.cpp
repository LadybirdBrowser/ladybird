/*
 * Copyright (c) 2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/File.h>

namespace IPC {

File File::adopt_file(NonnullOwnPtr<Core::File> file)
{
    return File(file->leak_fd());
}

File File::adopt_fd(int fd)
{
    return File(fd);
}

ErrorOr<File> File::clone_fd(int fd)
{
    int new_fd = TRY(Core::System::dup(fd));
    return File(new_fd);
}

File::File(int fd)
    : m_fd(fd)
{
}

File::File(File&& other)
    : m_fd(exchange(other.m_fd, -1))
{
}

File& File::operator=(File&& other)
{
    if (this != &other) {
        m_fd = exchange(other.m_fd, -1);
    }
    return *this;
}

File::~File()
{
    if (m_fd != -1)
        (void)Core::System::close(m_fd);
}

// FIXME: IPC::Files transferred over the wire always set O_CLOEXEC during decoding. Perhaps we should add an option to
//        allow the receiver to decide whether to make it O_CLOEXEC or not. Or an attribute in the .ipc file?
ErrorOr<void> File::clear_close_on_exec()
{
    return Core::System::set_close_on_exec(m_fd, false);
}

template<>
ErrorOr<File> decode(Decoder& decoder)
{
    auto file = decoder.files().dequeue();
    TRY(Core::System::set_close_on_exec(file.fd(), true));
    return file;
}

}
