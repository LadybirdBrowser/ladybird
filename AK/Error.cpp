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
Error Error::from_windows_error(u32 windows_error)
{
    return Error(static_cast<int>(windows_error), Error::Kind::Windows);
}

// This can be used both for generic Windows errors and for winsock errors because WSAGetLastError is forwarded to GetLastError.
Error Error::from_windows_error()
{
    return from_windows_error(GetLastError());
}
#endif

}
