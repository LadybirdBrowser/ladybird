/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/FFIHelpers.h>
#include <AK/FlyString.h>
#include <AK/Forward.h>
#include <AK/String.h>
#include <AK/StringConversions.h>
#include <AK/StringView.h>
#include <AK/Try.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/kmalloc.h>

#include <simdutf.h>

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

extern "C" bool ladybird_utf16_validate(u16 const* data, size_t length)
{
    VERIFY(data != nullptr);
    return AK::Utf16View { reinterpret_cast<char16_t const*>(data), length }.validate();
}

extern "C" bool ladybird_utf16_validate_prefix(u16 const* data, size_t length, size_t* valid_code_units)
{
    VERIFY(data != nullptr);
    return AK::Utf16View { reinterpret_cast<char16_t const*>(data), length }.validate(*valid_code_units);
}

extern "C" size_t ladybird_convert_valid_utf16_to_utf8(u16 const* data, size_t length, u8* output)
{
    return simdutf::convert_valid_utf16_to_utf8(reinterpret_cast<char16_t const*>(data), length, reinterpret_cast<char*>(output));
}

extern "C" FlatPtr ladybird_utf16_string_from_utf8(u8 const* data, size_t length)
{
    return AK::Utf16String::from_utf8_without_validation(AK::ffi_string_view(data, length)).into_raw();
}

extern "C" FlatPtr ladybird_utf16_string_to_well_formed(u16 const* data, size_t length)
{
    VERIFY(data != nullptr);
    auto string = AK::Detail::Utf16StringData::to_well_formed({ reinterpret_cast<char16_t const*>(data), length });
    return reinterpret_cast<FlatPtr>(&string.leak_ref());
}

extern "C" size_t ladybird_size_required_to_decode_base64(void const* data, size_t length, bool has_ascii_storage)
{
    if (has_ascii_storage)
        return AK::size_required_to_decode_base64(AK::ffi_string_view(static_cast<u8 const*>(data), length));
    return AK::size_required_to_decode_base64(AK::Utf16View { static_cast<char16_t const*>(data), length });
}

extern "C" u8 ladybird_decode_base64_into(void const* data, size_t length, bool has_ascii_storage, bool url, u8 last_chunk_handling, u8* output, size_t* output_length, size_t* read)
{
    auto handling = static_cast<AK::LastChunkHandling>(last_chunk_handling);
    Bytes bytes { output, *output_length };

    auto result = [&] {
        if (has_ascii_storage) {
            auto input = AK::ffi_string_view(static_cast<u8 const*>(data), length);
            return url ? AK::decode_base64url_into(input, bytes, handling) : AK::decode_base64_into(input, bytes, handling);
        }
        AK::Utf16View input { static_cast<char16_t const*>(data), length };
        return url ? AK::decode_base64url_into(input, bytes, handling) : AK::decode_base64_into(input, bytes, handling);
    }();

    *output_length = bytes.size();
    if (result.is_error()) {
        *read = result.error().valid_input_bytes;
        return to_underlying(result.error().decode_error) + 1;
    }
    *read = result.value();
    return 0;
}

extern "C" FlatPtr ladybird_encode_base64_to_utf16(u8 const* data, size_t length, bool url, bool omit_padding)
{
    ReadonlyBytes input { data, length };
    auto padding = omit_padding ? AK::OmitPadding::Yes : AK::OmitPadding::No;
    auto string = MUST(url ? AK::encode_base64url_to_utf16(input, padding) : AK::encode_base64_to_utf16(input, padding));
    return move(string).into_raw();
}

extern "C" void ladybird_convert_to_decimal_exponential_form(double value, bool* sign, u64* fraction, i32* exponent)
{
    auto form = AK::convert_to_decimal_exponential_form(value);
    *sign = form.sign;
    *fraction = form.fraction;
    *exponent = form.exponent;
}

extern "C" void* ladybird_alloc(size_t size, size_t alignment)
{
    // NB: mimalloc only guarantees natural alignment up to the allocation size.
    if (alignment <= alignof(max_align_t))
        return ak_kmalloc(max(size, alignment));

    Checked<size_t> allocation_size = size;
    allocation_size += alignment - 1;
    allocation_size += sizeof(void*);
    if (allocation_size.has_overflow())
        return nullptr;
    auto* allocation = ak_kmalloc(allocation_size.value());
    if (!allocation)
        return nullptr;
    auto aligned_address = align_up_to(reinterpret_cast<FlatPtr>(allocation) + sizeof(void*), alignment);
    auto* pointer = reinterpret_cast<void**>(aligned_address);
    pointer[-1] = allocation;
    return pointer;
}

extern "C" void* ladybird_alloc_zeroed(size_t size, size_t alignment)
{
    if (alignment <= alignof(max_align_t))
        return ak_kcalloc(1, max(size, alignment));
    auto* pointer = ladybird_alloc(size, alignment);
    if (pointer)
        __builtin_memset(pointer, 0, size);
    return pointer;
}

extern "C" void ladybird_dealloc(void* pointer, size_t alignment)
{
    if (pointer && alignment > alignof(max_align_t))
        pointer = static_cast<void**>(pointer)[-1];
    ak_kfree(pointer);
}

extern "C" void* ladybird_realloc(void* pointer, size_t old_size, size_t new_size, size_t alignment)
{
    if (alignment <= alignof(max_align_t))
        return ak_krealloc(pointer, max(new_size, alignment));
    auto* new_pointer = ladybird_alloc(new_size, alignment);
    if (new_pointer) {
        __builtin_memcpy(new_pointer, pointer, min(old_size, new_size));
        ladybird_dealloc(pointer, alignment);
    }
    return new_pointer;
}
