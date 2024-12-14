/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/StringView.h>

#ifndef AK_OS_WINDOWS
#    include <cxxabi.h>
#endif

namespace AK {

#ifndef AK_OS_WINDOWS
inline ByteString demangle(StringView name)
{
    int status = 0;
    auto* demangled_name = abi::__cxa_demangle(name.to_byte_string().characters(), nullptr, nullptr, &status);
    auto string = ByteString(status == 0 ? StringView { demangled_name, strlen(demangled_name) } : name);
    if (status == 0)
        free(demangled_name);
    return string;
}
#else
inline ByteString demangle(StringView name)
{
    // FIXME: Implement AK::demangle on Windows
    return name;
}
#endif

}

#if USING_AK_GLOBALLY
using AK::demangle;
#endif
