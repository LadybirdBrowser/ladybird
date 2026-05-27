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

#ifdef USING_AK_GLOBALLY
using AK::ffi_fly_string;
using AK::ffi_string;
using AK::ffi_string_view;
#endif
