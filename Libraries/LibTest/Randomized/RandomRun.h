/*
 * Copyright (c) 2023, Martin Janiczek <martin@janiczek.cz>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/QuickSort.h>
#include <AK/Vector.h>
#include <LibTest/Randomized/Chunk.h>

namespace Test {
namespace Randomized {

// RandomRun is a record of random bits used in generation of random values.
// Once a value failing user test is found, we then attempt to shrink its
// RandomRun using various ShrinkCommands.
//
// This means that we construct new RandomRuns by saying "OK, but what if the
// PRNG gave you 0 instead of 23 that time..." The runner then tries to
// generate a new value from the new RandomRun; if it succeeds and the value
// still fails the test, we've shrunk our counterexample some!
//
// RandomRun is conceptually a sequence of unsigned integers, e.g.
// [5,3,10,8,0,0,1].
class RandomRun {
public:
    RandomRun() = default;
    RandomRun(RandomRun const& rhs) = default;
    RandomRun& operator=(RandomRun const& rhs) = default;
    explicit RandomRun(Vector<u64> const& data)
        : m_data(move(data))
    {
    }
    bool is_empty() const { return m_data.is_empty(); }
    bool contains_chunk(Chunk const& c) const { return c.index + c.size <= m_data.size(); }
    void append(u64 n) { m_data.append(n); }
    size_t size() const { return m_data.size(); }
    Optional<u64> next()
    {
        if (m_current_index < m_data.size()) {
            return m_data[m_current_index++];
        }
        return Optional<u64> {};
    }
    u64& operator[](size_t index) { return m_data[index]; }
    u64 const& operator[](size_t index) const { return m_data[index]; }
    Vector<u64> data() const { return m_data; }

    // Shortlex sorting
    //
    // This is the metric by which we try to minimize (shrink) the sequence of
    // random choices, from which we later generate values.
    //
    // Shorter is better; if the length is equal then lexicographic order is
    // used. See [paper], section 2.2.
    //
    // Examples:
    // [9,9,9] < [0,0,0,0] (shorter is better)
    // [8,9,9] < [9,0,0] (lexicographic ordering: numbers that appear earlier
    //                    are more "important" than numbers that follow them)
    //
    // [paper]: https://drops.dagstuhl.de/opus/volltexte/2020/13170/
    bool is_shortlex_smaller_than(RandomRun const& rhs) const
    {
        auto lhs_size = size();
        auto rhs_size = rhs.size();

        if (lhs_size < rhs_size)
            return true;

        if (lhs_size > rhs_size)
            return false;

        for (size_t i = 0; i < lhs_size; i++) {
            if (m_data[i] < rhs.m_data[i])
                return true;

            if (m_data[i] > rhs.m_data[i])
                return false;
        }
        return false;
    }

    RandomRun with_sorted(Chunk c) const
    {
        Vector<u64> new_data = m_data;
        AK::dual_pivot_quick_sort(
            new_data,
            c.index,
            c.index + c.size - 1,
            [](auto& a, auto& b) { return a < b; });
        return RandomRun(move(new_data));
    }
    RandomRun with_deleted(Chunk c) const
    {
        Vector<u64> new_data(m_data);
        new_data.remove(c.index, c.size);
        return RandomRun(move(new_data));
    }

private:
    Vector<u64> m_data;
    size_t m_current_index = 0;
};

} // namespace Randomized
} // namespace Test

template<>
struct AK::Formatter<Test::Randomized::RandomRun> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Test::Randomized::RandomRun run)
    {
        return Formatter<StringView>::format(builder, TRY(String::formatted("[{}]"sv, TRY(String::join(',', run.data(), "{}"sv)))));
    }
};
