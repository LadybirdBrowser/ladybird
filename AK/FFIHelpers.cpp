/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FFIHelpers.h>
#include <AK/FlyString.h>
#include <AK/Forward.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Try.h>

namespace AK {

FlyString ffi_fly_string(u8 const* ptr, size_t len)
{
    return MUST(FlyString::from_utf8(ffi_string_view(ptr, len)));
}

String ffi_string(u8 const* ptr, size_t len)
{
    return MUST(String::from_utf8(ffi_string_view(ptr, len)));
}

StringView ffi_string_view(u8 const* ptr, size_t len)
{
    // NOTE: A zero length C string is valid
    if (ptr == nullptr)
        return {};
    return { ptr, len };
}

}
