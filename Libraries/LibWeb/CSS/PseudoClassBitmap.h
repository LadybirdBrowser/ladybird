/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/BuiltinWrappers.h>
#include <LibWeb/CSS/PseudoClass.h>

namespace Web::CSS {

class PseudoClassBitmap {
public:
    static constexpr size_t bits_per_word = 64;
    static constexpr size_t word_count = (to_underlying(PseudoClass::__Count) + bits_per_word - 1) / bits_per_word;

    PseudoClassBitmap() = default;
    ~PseudoClassBitmap() = default;

    void set(PseudoClass pseudo_class, bool bit)
    {
        size_t const index = to_underlying(pseudo_class);
        size_t const word_index = index / bits_per_word;
        size_t const bit_index = index % bits_per_word;
        if (bit)
            m_bits[word_index] |= 1LLU << bit_index;
        else
            m_bits[word_index] &= ~(1LLU << bit_index);
    }

    bool get(PseudoClass pseudo_class) const
    {
        size_t const index = to_underlying(pseudo_class);
        size_t const word_index = index / bits_per_word;
        size_t const bit_index = index % bits_per_word;
        return (m_bits[word_index] & (1LLU << bit_index)) != 0;
    }

    template<typename Callback>
    void for_each_set(Callback callback) const
    {
        for (size_t word_index = 0; word_index < word_count; ++word_index) {
            for (u64 word = m_bits[word_index]; word != 0; word &= word - 1)
                callback(static_cast<PseudoClass>(word_index * bits_per_word + count_trailing_zeroes(word)));
        }
    }

    void operator|=(PseudoClassBitmap const& other)
    {
        for (size_t i = 0; i < word_count; ++i)
            m_bits[i] |= other.m_bits[i];
    }

private:
    Array<u64, word_count> m_bits {};
};

}
