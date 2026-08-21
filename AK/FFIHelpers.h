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

FlatPtr ladybird_utf16_string_create_uninitialized(size_t, bool has_ascii_storage);
FlatPtr ladybird_utf16_fly_string_from_utf8(u8 const*, size_t);
FlatPtr ladybird_utf16_fly_string_from_utf16(u16 const*, size_t);
void ladybird_utf16_string_unref(FlatPtr);
}

#ifdef USING_AK_GLOBALLY
using AK::ffi_fly_string;
using AK::ffi_string;
using AK::ffi_string_view;
#endif
