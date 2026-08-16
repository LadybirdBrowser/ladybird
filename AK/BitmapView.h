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

    [[nodiscard]] size_t count_slow(bool value) const
    {
        return count_in_range(0, m_size, value);
    }

    [[nodiscard]] size_t count_in_range(size_t start, size_t len, bool value) const
    {
        VERIFY(start < m_size);
        VERIFY(start + len <= m_size);
        if (len == 0)
            return 0;

        size_t count;
        u8 const* first = &m_data[start / 8];
        u8 const* last = &m_data[(start + len) / 8];
        u8 byte = *first;
        byte &= bitmask_first_byte[start % 8];
        if (first == last) {
            byte &= bitmask_last_byte[(start + len) % 8];
            count = popcount(byte);
        } else {
            count = popcount(byte);
            // Don't access *last if it's out of bounds
            if (last < &m_data[size_in_bytes()]) {
                byte = *last;
                byte &= bitmask_last_byte[(start + len) % 8];
                count += popcount(byte);
            }
            if (++first < last) {
                size_t const* ptr_large = reinterpret_cast<size_t const*>((reinterpret_cast<FlatPtr>(first) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1));
                if (reinterpret_cast<u8 const*>(ptr_large) > last)
                    ptr_large = reinterpret_cast<size_t const*>(last);
                while (first < reinterpret_cast<u8 const*>(ptr_large)) {
                    count += popcount(*first);
                    first++;
                }
                size_t const* last_large = reinterpret_cast<size_t const*>(reinterpret_cast<FlatPtr>(last) & ~(sizeof(size_t) - 1));
                while (ptr_large < last_large) {
                    count += popcount(*ptr_large);
                    ptr_large++;
                }
                for (first = reinterpret_cast<u8 const*>(ptr_large); first < last; first++)
                    count += popcount(*first);
            }
        }

        if (!value)
            count = len - count;
        return count;
    }

    [[nodiscard]] bool is_null() const { return m_data == nullptr; }

    [[nodiscard]] u8 const* data() const { return m_data; }

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

    // The function will return the next range of unset bits starting from the
    // @from value.
    // @from: the position from which the search starts. The var will be
    //        changed and new value is the offset of the found block.
    // @min_length: minimum size of the range which will be returned.
    // @max_length: maximum size of the range which will be returned.
    //              This is used to increase performance, since the range of
    //              unset bits can be long, and we don't need the while range,
    //              so we can stop when we've reached @max_length.
    inline Optional<size_t> find_next_range_of_unset_bits(size_t& from, size_t min_length = 1, size_t max_length = max_size) const
    {
        if (min_length > max_length) {
            return {};
        }

        size_t bit_size = 8 * sizeof(size_t);

        size_t* bitmap = reinterpret_cast<size_t*>(m_data);

        // Calculating the start offset.
        size_t start_bucket_index = from / bit_size;
        size_t start_bucket_bit = from % bit_size;

        size_t* start_of_free_chunks = &from;
        size_t free_chunks = 0;

        for (size_t bucket_index = start_bucket_index; bucket_index < m_size / bit_size; ++bucket_index) {
            if (bitmap[bucket_index] == NumericLimits<size_t>::max()) {
                // Skip over completely full bucket of size bit_size.
                if (free_chunks >= min_length) {
                    return min(free_chunks, max_length);
                }
                free_chunks = 0;
                start_bucket_bit = 0;
                continue;
            }
            if (bitmap[bucket_index] == 0x0) {
                // Skip over completely empty bucket of size bit_size.
                if (free_chunks == 0) {
                    *start_of_free_chunks = bucket_index * bit_size;
                }
                free_chunks += bit_size;
                if (free_chunks >= max_length) {
                    return max_length;
                }
                start_bucket_bit = 0;
                continue;
            }

            size_t bucket = bitmap[bucket_index];
            u8 viewed_bits = start_bucket_bit;
            u32 trailing_zeroes = 0;

            bucket >>= viewed_bits;
            start_bucket_bit = 0;

            while (viewed_bits < bit_size) {
                if (bucket == 0) {
                    if (free_chunks == 0) {
                        *start_of_free_chunks = bucket_index * bit_size + viewed_bits;
                    }
                    free_chunks += bit_size - viewed_bits;
                    viewed_bits = bit_size;
                } else {
                    trailing_zeroes = count_trailing_zeroes(bucket);
                    bucket >>= trailing_zeroes;

                    if (free_chunks == 0) {
                        *start_of_free_chunks = bucket_index * bit_size + viewed_bits;
                    }
                    free_chunks += trailing_zeroes;
                    viewed_bits += trailing_zeroes;

                    if (free_chunks >= min_length) {
                        return min(free_chunks, max_length);
                    }

                    // Deleting trailing ones.
                    u32 trailing_ones = count_trailing_zeroes(~bucket);
                    bucket >>= trailing_ones;
                    viewed_bits += trailing_ones;
                    free_chunks = 0;
                }
            }
        }

        if (free_chunks < min_length) {
            size_t first_trailing_bit = (m_size / bit_size) * bit_size;
            size_t trailing_bits = size() % bit_size;
            for (size_t i = 0; i < trailing_bits; ++i) {
                if (!get(first_trailing_bit + i)) {
                    if (free_chunks == 0)
                        *start_of_free_chunks = first_trailing_bit + i;
                    if (++free_chunks >= min_length)
                        return min(free_chunks, max_length);
                } else {
                    free_chunks = 0;
                }
            }
            return {};
        }

        return min(free_chunks, max_length);
    }

    Optional<size_t> find_longest_range_of_unset_bits(size_t max_length, size_t& found_range_size) const
    {
        size_t start = 0;
        size_t max_region_start = 0;
        size_t max_region_size = 0;

        while (true) {
            // Look for the next block which is bigger than currunt.
            auto length_of_found_range = find_next_range_of_unset_bits(start, max_region_size + 1, max_length);
            if (length_of_found_range.has_value()) {
                max_region_start = start;
                max_region_size = length_of_found_range.value();
                start += max_region_size;
            } else {
                // No ranges which are bigger than current were found.
                break;
            }
        }

        found_range_size = max_region_size;
        if (max_region_size != 0) {
            return max_region_start;
        }
        return {};
    }

    Optional<size_t> find_first_fit(size_t minimum_length) const
    {
        size_t start = 0;
        auto length_of_found_range = find_next_range_of_unset_bits(start, minimum_length, minimum_length);
        if (length_of_found_range.has_value()) {
            return start;
        }
        return {};
    }

    Optional<size_t> find_best_fit(size_t minimum_length) const
    {
        size_t start = 0;
        size_t best_region_start = 0;
        size_t best_region_size = max_size;
        bool found = false;

        while (true) {
            // Look for the next block which is bigger than requested length.
            auto length_of_found_range = find_next_range_of_unset_bits(start, minimum_length, best_region_size);
            if (length_of_found_range.has_value()) {
                if (best_region_size > length_of_found_range.value() || !found) {
                    best_region_start = start;
                    best_region_size = length_of_found_range.value();
                    found = true;
                }
                start += length_of_found_range.value();
            } else {
                // There are no ranges which can fit requested length.
                break;
            }
        }

        if (found) {
            return best_region_start;
        }
        return {};
    }

    static constexpr size_t max_size = 0xffffffff;

    [[nodiscard]] bool operator==(BitmapView const& other) const
    {
        if (size() != other.size())
            return false;
        for (size_t i = 0; i < size_in_bytes(); ++i) {
            if (m_data[i] != other.m_data[i])
                return false;
        }
        return true;
    }

protected:
    u8* m_data { nullptr };
    size_t m_size { 0 };
};

}

#if USING_AK_GLOBALLY
using AK::BitmapView;
#endif
