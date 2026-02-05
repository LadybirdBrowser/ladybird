/*
 * Copyright (c) 2023, Martin Janiczek <martin@janiczek.cz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibTest/Randomized/RandomRun.h>
#include <LibTest/TestResult.h>

namespace Test {
namespace Randomized {

// RandomnessSource provides random bits to Generators.
//
// If it's live, a PRNG will be used and the random values will be recorded into
// its RandomRun.
//
// If it's recorded, its RandomRun will be used to "mock" the PRNG. This allows
// us to replay the generation of a particular value, and to test out
// "alternative histories": "what if the PRNG generated 0 instead of 13 here?"
class RandomnessSource {
public:
    static RandomnessSource live() { return RandomnessSource(RandomRun(), true); }
    static RandomnessSource recorded(RandomRun const& run) { return RandomnessSource(run, false); }
    RandomRun& run() { return m_run; }
    u64 draw_value(u64 max, Function<u64()> random_generator)
    {
        // Live: use the random generator and remember the value.
        if (m_is_live) {
            u64 value = random_generator();
            m_run.append(value);
            return value;
        }

        // Not live! let's get another prerecorded value.
        auto next = m_run.next();
        if (next.has_value()) {
            return min(next.value(), max);
        }

        // Signal a failure. The value returned doesn't matter at this point but
        // we need to return something.
        set_current_test_result(TestResult::Overrun);
        return 0;
    }

private:
    explicit RandomnessSource(RandomRun const& run, bool is_live)
        : m_run(run)
        , m_is_live(is_live)
    {
    }
    RandomRun m_run;
    bool m_is_live;
};

} // namespace Randomized
} // namespace Test
