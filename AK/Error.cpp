/*
 * Copyright (c) 2023, Liav A. <liavalb@hotmail.co.il>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024-2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#ifdef AK_OS_WINDOWS
#    include <AK/ByteString.h>
#    include <AK/HashMap.h>
#    include <windows.h>
// Comment to prevent clang-format from including windows.h too late
#    include <winbase.h>
#endif

namespace AK {

Error Error::from_string_view_or_print_error_and_return_errno(StringView string_literal, [[maybe_unused]] int code)
{
    return Error::from_string_view(string_literal);
}

#ifdef AK_OS_WINDOWS
Error Error::from_windows_error(u64 code)
{
    thread_local HashMap<u64, ByteString> s_windows_errors;

    auto string = s_windows_errors.get(code);
    if (string.has_value())
        return Error::from_string_view(string->view());

    char* message = nullptr;
    auto size = FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(code),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&message),
        0,
        nullptr);

    if (size == 0) {
        static char buffer[128];
        (void)snprintf(buffer, _countof(buffer), "Error 0x%08lX while getting text of error 0x%08llX", GetLastError(), code);
        return Error::from_string_view({ buffer, _countof(buffer) });
    }

    auto& string_in_map = s_windows_errors.ensure(code, [message, size] { return ByteString { message, size }; });
    LocalFree(message);
    return Error::from_string_view(string_in_map.view());
}

// This can be used both for generic Windows errors and for winsock errors because WSAGetLastError is forwarded to GetLastError.
Error Error::from_windows_error()
{
    return from_windows_error(GetLastError());
}
#endif

}
