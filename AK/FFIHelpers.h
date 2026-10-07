/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>

namespace AK {

FlyString ffi_fly_string(u8 const* ptr, size_t len);

String ffi_string(u8 const* ptr, size_t len);

StringView ffi_string_view(u8 const* ptr, size_t len);

}

extern "C" {

// Ordinary allocations can be handed to ak_kfree. Over-aligned allocations must retain their alignment
// and use ladybird_dealloc, which recovers the original AK allocation.
void* ladybird_alloc(size_t size, size_t alignment);
void* ladybird_alloc_zeroed(size_t size, size_t alignment);
void* ladybird_realloc(void*, size_t old_size, size_t new_size, size_t alignment);
void ladybird_dealloc(void*, size_t alignment);

FlatPtr ladybird_utf16_string_create_uninitialized(size_t, bool has_ascii_storage);
FlatPtr ladybird_utf16_fly_string_from_utf8(u8 const*, size_t);
FlatPtr ladybird_utf16_fly_string_from_utf16(u16 const*, size_t);
void ladybird_utf16_string_unref(FlatPtr);
bool ladybird_utf16_validate(u16 const*, size_t);
bool ladybird_utf16_validate_prefix(u16 const*, size_t, size_t* valid_code_units);
size_t ladybird_convert_valid_utf16_to_utf8(u16 const*, size_t, u8* output);
FlatPtr ladybird_utf16_string_from_utf8(u8 const*, size_t);
FlatPtr ladybird_utf16_string_to_well_formed(u16 const*, size_t);

size_t ladybird_size_required_to_decode_base64(void const*, size_t, bool has_ascii_storage);
u8 ladybird_decode_base64_into(void const*, size_t, bool has_ascii_storage, bool url, u8 last_chunk_handling, u8* output, size_t* output_length, size_t* read);
FlatPtr ladybird_encode_base64_to_utf16(u8 const*, size_t, bool url, bool omit_padding);

void ladybird_convert_to_decimal_exponential_form(double, bool* sign, u64* fraction, i32* exponent);
}

#ifdef USING_AK_GLOBALLY
using AK::ffi_fly_string;
using AK::ffi_string;
using AK::ffi_string_view;
#endif
