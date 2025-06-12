/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Traits.h>

namespace AK {

template<typename T>
class TypedTransfer {
public:
    static void move(T* destination, T* source, size_t count)
    {
        if (count == 0)
            return;

        if constexpr (Traits<T>::is_trivial()) {
            __builtin_memmove(destination, source, count * sizeof(T));
            return;
        }

        for (size_t i = 0; i < count; ++i) {
            if (destination <= source)
                new (&destination[i]) T(AK::move(source[i]));
            else
                new (&destination[count - i - 1]) T(AK::move(source[count - i - 1]));
        }
    }

    static size_t copy(T* destination, T const* source, size_t count)
    {
        if (count == 0)
            return 0;

        if constexpr (Traits<T>::is_trivial()) {
            if (count == 1)
                *destination = *source;
            else
                __builtin_memmove(destination, source, count * sizeof(T));
            return count;
        }

        for (size_t i = 0; i < count; ++i) {
            if (destination <= source)
                new (&destination[i]) T(source[i]);
            else
                new (&destination[count - i - 1]) T(source[count - i - 1]);
        }

        return count;
    }

    static bool compare(T const* a, T const* b, size_t count)
    {
        if (count == 0)
            return true;

        if constexpr (Traits<T>::is_trivial())
            return !__builtin_memcmp(a, b, count * sizeof(T));

        for (size_t i = 0; i < count; ++i) {
            if (a[i] != b[i])
                return false;
        }

        return true;
    }

    static void delete_(T* ptr, size_t count)
    {
        if (count == 0)
            return;

        if constexpr (Traits<T>::is_trivial()) {
            return;
        }

        for (size_t i = 0; i < count; ++i)
            ptr[i].~T();
    }
};

}
