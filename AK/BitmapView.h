/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace AK {

static constexpr Array bitmask_first_byte = { 0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE0, 0xC0, 0x80 };
static constexpr Array bitmask_last_byte = { 0x00, 0x1, 0x3, 0x7, 0xF, 0x1F, 0x3F, 0x7F };

class BitmapView {
public:
    BitmapView() = default;

    BitmapView(u8* data, size_t size)
        : m_data(data)
        , m_size(size)
    {
    }

    [[nodiscard]] size_t size() const { return m_size; }
    [[nodiscard]] size_t size_in_bytes() const { return ceil_div(m_size, static_cast<size_t>(8)); }
    [[nodiscard]] bool get(size_t index) const
    {
        VERIFY(index < m_size);
        return 0 != (m_data[index / 8] & (1u << (index % 8)));
    }

    template<bool VALUE>
    Optional<size_t> find_one_anywhere(size_t hint = 0) const
    {
        VERIFY(hint < m_size);
        auto find_in_range = [&](size_t start, size_t end) -> Optional<size_t> {
            if (start == end)
                return {};

            auto first_byte = start / 8;
            auto last_byte = (end - 1) / 8;
            auto find_in_byte = [&](size_t i, u8 mask = 0xff) -> Optional<size_t> {
                u8 matching_bits = VALUE ? m_data[i] : static_cast<u8>(~m_data[i]);
                matching_bits &= mask;
                if (matching_bits != 0)
                    return i * 8 + bit_scan_forward(matching_bits) - 1;
                return {};
            };

            if (first_byte == last_byte) {
                auto mask = bitmask_first_byte[start % 8];
                if (end % 8 != 0)
                    mask &= bitmask_last_byte[end % 8];
                return find_in_byte(first_byte, mask);
            }

            if (start % 8 != 0) {
                if (auto result = find_in_byte(first_byte, bitmask_first_byte[start % 8]); result.has_value())
                    return result;
                ++first_byte;
            }

            auto full_byte_end = end / 8;
            while (first_byte < full_byte_end && reinterpret_cast<FlatPtr>(&m_data[first_byte]) % alignof(size_t) != 0) {
                if (auto result = find_in_byte(first_byte); result.has_value())
                    return result;
                ++first_byte;
            }

            while (full_byte_end - first_byte >= sizeof(size_t)) {
                auto word = *reinterpret_cast<size_t const*>(&m_data[first_byte]);
                auto matching_bits = VALUE ? word : ~word;
                if (matching_bits != 0) {
                    for (size_t i = 0; i < sizeof(size_t); ++i) {
                        if (auto result = find_in_byte(first_byte + i); result.has_value())
                            return result;
                    }
                    VERIFY_NOT_REACHED();
                }
                first_byte += sizeof(size_t);
            }

            while (first_byte < full_byte_end) {
                if (auto result = find_in_byte(first_byte); result.has_value())
                    return result;
                ++first_byte;
            }

            if (end % 8 != 0)
                return find_in_byte(full_byte_end, bitmask_last_byte[end % 8]);
            return {};
        };

        auto bits_per_word = sizeof(size_t) * 8;
        auto search_start = hint / bits_per_word * bits_per_word;
        if (auto result = find_in_range(search_start, m_size); result.has_value())
            return result;
        return find_in_range(0, search_start);
    }

    Optional<size_t> find_one_anywhere_set(size_t hint = 0) const
    {
        return find_one_anywhere<true>(hint);
    }

    Optional<size_t> find_one_anywhere_unset(size_t hint = 0) const
    {
        return find_one_anywhere<false>(hint);
    }

    template<bool VALUE>
    Optional<size_t> find_first() const
    {
        if (m_size == 0)
            return {};
        return find_one_anywhere<VALUE>();
    }

    Optional<size_t> find_first_set() const { return find_first<true>(); }
    Optional<size_t> find_first_unset() const { return find_first<false>(); }

protected:
    u8* m_data { nullptr };
    size_t m_size { 0 };
};

}

#if USING_AK_GLOBALLY
using AK::BitmapView;
#endif
