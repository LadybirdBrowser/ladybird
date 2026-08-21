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
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>

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

extern "C" FlatPtr ladybird_utf16_string_create_uninitialized(size_t length, bool has_ascii_storage)
{
    auto storage_type = has_ascii_storage
        ? AK::Detail::Utf16StringData::StorageType::ASCII
        : AK::Detail::Utf16StringData::StorageType::UTF16;
    auto data = AK::Detail::Utf16StringData::create_uninitialized_for_ffi(storage_type, length);
    return reinterpret_cast<FlatPtr>(&data.leak_ref());
}

extern "C" FlatPtr ladybird_utf16_fly_string_from_utf8(u8 const* data, size_t length)
{
    VERIFY(data != nullptr || length == 0);
    return AK::Utf16FlyString::from_utf8(AK::ffi_string_view(data, length)).into_raw();
}

extern "C" FlatPtr ladybird_utf16_fly_string_from_utf16(u16 const* data, size_t length)
{
    if (data == nullptr) {
        VERIFY(length == 0);
        return AK::Utf16FlyString {}.into_raw();
    }
    return AK::Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(data), length }).into_raw();
}

extern "C" void ladybird_utf16_string_unref(FlatPtr raw)
{
    AK::Utf16String::unref_raw(raw);
}
