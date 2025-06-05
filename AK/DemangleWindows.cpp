/*
 * Copyright (c) 2025, Tomasz Strejczek
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/StringView.h>

#include <Windows.h>
#include <dbghelp.h>

#pragma comment(lib, "Dbghelp.lib") // /DELAYLOAD:Dbghelp.dll is configured in CMakeLists.txt

#include <AK/Demangle.h>

namespace AK {

ByteString demangle(StringView name)
{
    // Buffer to hold the demangled name
    // The buffer size is arbitrary but should be large enough for most cases.
    // Unfortunately, there is no way to know the exact size needed beforehand.
    // Also calling UnDecorateSymbolName with a too small buffer will not return
    // an error, it will just truncate the result.
    char buffer[128] = {};

    // UNDNAME_COMPLETE asks for the full decoration (equivalent to g++’s default
    // demangle)
    auto chars_written = UnDecorateSymbolName(name.to_byte_string().characters(), buffer,
        sizeof(buffer), UNDNAME_COMPLETE);

    return ByteString(chars_written > 0
            ? StringView { buffer, static_cast<size_t>(chars_written) }
            : name);
}

} // namespace AK
