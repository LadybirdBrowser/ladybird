/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AllOf.h>
#include <AK/Array.h>
#include <AK/NumericLimits.h>
#include <AK/Types.h>

namespace Web::DOM {

// A bloom filter of names by their hashes. Names only ever get added: one no element holds any more costs a selector
// query the walk it would have skipped.
class ElementNameFilter {
public:
    void add(u32 hash)
    {
        set_bit(first_bit(hash));
        set_bit(second_bit(hash));
    }

    [[nodiscard]] bool may_contain(u32 hash) const
    {
        return has_bit(first_bit(hash)) && has_bit(second_bit(hash));
    }

    // Make every name possible, unless no name was added.
    void saturate_unless_empty()
    {
        if (all_of(m_words, [](u64 word) { return word == 0; }))
            return;
        m_words.fill(NumericLimits<u64>::max());
    }

private:
    static constexpr size_t word_count = 16;
    static constexpr size_t bit_count = word_count * 64;

    static constexpr size_t first_bit(u32 hash) { return hash % bit_count; }
    static constexpr size_t second_bit(u32 hash) { return (hash >> 16) % bit_count; }

    void set_bit(size_t bit) { m_words[bit / 64] |= 1ull << (bit % 64); }
    [[nodiscard]] bool has_bit(size_t bit) const { return (m_words[bit / 64] & (1ull << (bit % 64))) != 0; }

    Array<u64, word_count> m_words {};
};

}
