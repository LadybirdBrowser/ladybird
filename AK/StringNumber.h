/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace AK {

template<class StringType, Integral T>
StringType create_string_from_number(T value)
{
    // Maximum number of base-10 digits for T + sign
    constexpr size_t max_digits = sizeof(T) * 3 + 2;
    char buffer[max_digits];
    char* ptr = buffer + max_digits;
    bool is_negative = false;

    using UnsignedT = MakeUnsigned<T>;

    UnsignedT unsigned_value;
    if constexpr (IsSigned<T>) {
        if (value < 0) {
            is_negative = true;
            // Handle signed min correctly
            unsigned_value = static_cast<UnsignedT>(0) - static_cast<UnsignedT>(value);
        } else {
            unsigned_value = static_cast<UnsignedT>(value);
        }
    } else {
        unsigned_value = value;
    }

    if (unsigned_value == 0) {
        *--ptr = '0';
    } else {
        while (unsigned_value != 0) {
            *--ptr = '0' + (unsigned_value % 10);
            unsigned_value /= 10;
        }
    }

    if (is_negative) {
        *--ptr = '-';
    }

    size_t size = buffer + max_digits - ptr;
    return StringType::from_utf8_without_validation(ReadonlyBytes { ptr, size });
}

}
