/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

namespace AK {

template<typename T, typename U>
[[nodiscard]] constexpr ALWAYS_INLINE T bit_cast(U const& a)
{
#if (__has_builtin(__builtin_bit_cast))
    return __builtin_bit_cast(T, a);
#else
    static_assert(sizeof(T) == sizeof(U));

    T result;
    __builtin_memcpy(&result, &a, sizeof(T));
    return result;
#endif
}

}

#if USING_AK_GLOBALLY
using AK::bit_cast;
#endif
