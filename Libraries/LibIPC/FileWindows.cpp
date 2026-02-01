/*
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/File.h>
#include <LibIPC/HandleType.h>

#include <AK/Windows.h>

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
    auto handle_type = TRY(decoder.decode<HandleType>());
    int handle = -1;
    if (handle_type == HandleType::Generic) {
        TRY(decoder.decode_into(handle));
    } else if (handle_type == HandleType::Socket) {
        WSAPROTOCOL_INFOW pi = {};
        TRY(decoder.decode_into({ reinterpret_cast<u8*>(&pi), sizeof(pi) }));
        handle = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, &pi, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        if (handle == -1)
            return Error::from_windows_error();
    } else {
        return Error::from_string_literal("Invalid handle type");
    }
    return File::adopt_fd(handle);
}

}
