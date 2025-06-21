/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Utf16View.h>

namespace WebGPUNative {

template<size_t N>
Error make_error(HRESULT const result, char const (&message)[N])
{
    wchar_t* message_buffer = nullptr;
    ScopeGuard message_buffer_guard([&message_buffer] { LocalFree(message_buffer); });
    DWORD message_length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        result,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message_buffer),
        0,
        nullptr);

    auto const message_string = MUST(String::from_utf16(Utf16View { { reinterpret_cast<u16*>(message_buffer), message_length } }));
    auto const error_string = MUST(String::formatted("WebGPUNative [DirectX]: {}, {}", message, message_string.bytes_as_string_view()));
    return Error::from_string_view(error_string.bytes_as_string_view());
}

template<size_t N>
Error make_error(char const (&message)[N])
{
    auto const error_string = MUST(String::formatted("WebGPUNative [DirectX]: {}", message));
    return Error::from_string_view(error_string.bytes_as_string_view());
}

}
