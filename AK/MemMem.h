/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <AK/Vector.h>

namespace AK {

namespace Detail {

constexpr void const* bitap_bitwise(void const* haystack, size_t haystack_length, void const* needle, size_t needle_length)
{
    VERIFY(needle_length < 32);

    u32 lookup = 0xfffffffe;

    constexpr size_t mask_length = (size_t)((u8)-1) + 1;
    u32 needle_mask[mask_length];

    for (size_t i = 0; i < mask_length; ++i)
        needle_mask[i] = 0xffffffff;

    for (size_t i = 0; i < needle_length; ++i)
        needle_mask[((u8 const*)needle)[i]] &= ~(0x00000001 << i);

    for (size_t i = 0; i < haystack_length; ++i) {
        lookup |= needle_mask[((u8 const*)haystack)[i]];
        lookup <<= 1;

        if (0 == (lookup & (0x00000001 << needle_length)))
            return ((u8 const*)haystack) + i - needle_length + 1;
    }

    return nullptr;
}

}

template<typename HaystackIterT>
inline Optional<size_t> memmem(HaystackIterT const& haystack_begin, HaystackIterT const& haystack_end, ReadonlyBytes needle)
requires(requires { (*haystack_begin).data(); (*haystack_begin).size(); })
{
    auto prepare_kmp_partial_table = [&] {
        Vector<int, 64> table;
        table.resize(needle.size());

        size_t position = 1;
        int candidate = 0;

        table[0] = -1;
        while (position < needle.size()) {
            if (needle[position] == needle[candidate]) {
                table[position] = table[candidate];
            } else {
                table[position] = candidate;
                do {
                    candidate = table[candidate];
                } while (candidate >= 0 && needle[candidate] != needle[position]);
            }
            ++position;
            ++candidate;
        }
        return table;
    };

    auto table = prepare_kmp_partial_table();
    size_t total_haystack_index = 0;
    size_t current_haystack_index = 0;
    int needle_index = 0;
    auto haystack_it = haystack_begin;

    while (haystack_it != haystack_end) {
        auto&& chunk = *haystack_it;
        if (current_haystack_index >= chunk.size()) {
            current_haystack_index = 0;
            ++haystack_it;
            continue;
        }
        if (needle[needle_index] == chunk[current_haystack_index]) {
            ++needle_index;
            ++current_haystack_index;
            ++total_haystack_index;
            if ((size_t)needle_index == needle.size())
                return total_haystack_index - needle_index;
            continue;
        }
        needle_index = table[needle_index];
        if (needle_index < 0) {
            ++needle_index;
            ++current_haystack_index;
            ++total_haystack_index;
        }
    }

    return {};
}

inline Optional<size_t> memmem_optional(void const* haystack, size_t haystack_length, void const* needle, size_t needle_length)
{
    if (needle_length == 0)
        return 0;

    if (haystack_length < needle_length)
        return {};

    if (haystack_length == needle_length) {
        if (__builtin_memcmp(haystack, needle, haystack_length) == 0)
            return 0;
        return {};
    }

    if (needle_length < 32) {
        auto const* ptr = Detail::bitap_bitwise(haystack, haystack_length, needle, needle_length);
        if (ptr)
            return static_cast<size_t>((FlatPtr)ptr - (FlatPtr)haystack);
        return {};
    }

    // Fallback to KMP.
    Array<ReadonlyBytes, 1> spans { ReadonlyBytes { (u8 const*)haystack, haystack_length } };
    return memmem(spans.begin(), spans.end(), { (u8 const*)needle, needle_length });
}

inline void const* memmem(void const* haystack, size_t haystack_length, void const* needle, size_t needle_length)
{
    auto offset = memmem_optional(haystack, haystack_length, needle, needle_length);
    if (offset.has_value())
        return ((u8 const*)haystack) + offset.value();

    return nullptr;
}

}
