/*
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/File.h>
#include <LibIPC/HandleType.h>

#include <AK/Windows.h>

namespace IPC {

template<>
ErrorOr<File> decode(Decoder& decoder)
{
    auto handle_type = TRY(decoder.decode<HandleType>());
    int handle = -1;
    if (handle_type == HandleType::Generic) {
        TRY(decoder.decode_into(handle));
    } else if (handle_type == HandleType::Socket) {
        WSAPROTOCOL_INFO pi = {};
        TRY(decoder.decode_into({ reinterpret_cast<u8*>(&pi), sizeof(pi) }));
        handle = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, &pi, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
        if (handle == -1)
            return Error::from_windows_error();
    } else {
        return Error::from_string_literal("Invalid handle type");
    }
    return File::adopt_fd(handle);
}

}
