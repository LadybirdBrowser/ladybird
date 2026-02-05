/*
 * Copyright (c) 2023, Martin Janiczek <martin@janiczek.cz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibTest/Macros.h>
#include <LibTest/Randomized/RandomRun.h>

#include <AK/Function.h>
#include <AK/Random.h>
#include <AK/String.h>
#include <AK/StringView.h>

#include <math.h>

namespace Test {
namespace Randomized {

// Returns a random double value in range 0..1.
// This is not a generator. It is meant to be used inside RandomnessSource::draw_value().
// Based on: https://dotat.at/@/2023-06-23-random-double.html
inline f64 get_random_probability()
{
    return static_cast<f64>(AK::get_random<u64>() >> 11) * 0x1.0p-53;
}

// Generators take random bits from the RandomnessSource and return a value
// back.
//
// Example:
// - Gen::number_u64(5,10) --> 9, 7, 5, 10, 8, ...
namespace Gen {

// An unsigned integer generator.
//
// The minimum value will always be 0.
// The maximum value is given by user in the argument.
//
// Gen::number_u64(10) -> value 5, RandomRun [5]
//                     -> value 8, RandomRun [8]
//                     etc.
//
// Shrinks towards 0.
inline u64 number_u64(u64 max)
{
    if (max == 0)
        return 0;

    u64 random = Test::randomness_source().draw_value(max, [&]() {
        // `clamp` to guard against integer overflow
        u64 exclusive_bound = AK::clamp(max + 1, max, NumericLimits<u64>::max());
        return AK::get_random_uniform_64(exclusive_bound);
    });
    return random;
}

// An unsigned integer generator in a particular range.
//
// Gen::number_u64(3,10) -> value 3,  RandomRun [0]
//                       -> value 8,  RandomRun [5]
//                       -> value 10, RandomRun [7]
//                       etc.
//
// In case `min == max`, the RandomRun footprint will be smaller: no randomness
// is needed.
//
// Gen::number_u64(3,3) -> value 3, RandomRun [] (always)
//
// Shrinks towards the minimum.
inline u64 number_u64(u64 min, u64 max)
{
    VERIFY(max >= min);
    return number_u64(max - min) + min;
}

// Randomly (uniformly) selects a value out of the given arguments.
//
// Gen::one_of(20,5,10) --> value 20, RandomRun [0]
//                      --> value 5,  RandomRun [1]
//                      --> value 10, RandomRun [2]
//
// Shrinks towards the earlier arguments (above, towards 20).
template<typename... Ts>
requires(sizeof...(Ts) > 0)
CommonType<Ts...> one_of(Ts... choices)
{
    Vector<CommonType<Ts...>> choices_vec { choices... };

    constexpr size_t count = sizeof...(choices);
    size_t i = number_u64(count - 1);
    return choices_vec[i];
}

template<typename T>
struct Choice {
    i32 weight;
    T value;
};
// Workaround for clang bug fixed in clang 17
template<typename T>
Choice(i32, T) -> Choice<T>;

// Randomly (uniformly) selects a value out of the given weighted arguments.
//
// Gen::frequency(
//   Gen::Choice {5,999},
//   Gen::Choice {1,111},
// )
//     --> value 999 (5 out of 6 times), RandomRun [0]
//     --> value 111 (1 out of 6 times), RandomRun [1]
//
// Shrinks towards the earlier arguments (above, towards 'x').
template<typename... Ts>
requires(sizeof...(Ts) > 0)
CommonType<Ts...> frequency(Choice<Ts>... choices)
{
    Vector<Choice<CommonType<Ts...>>> choices_vec { choices... };

    u64 sum = 0;
    for (auto const& choice : choices_vec) {
        VERIFY(choice.weight > 0);
        sum += static_cast<u64>(choice.weight);
    }

    u64 target = number_u64(sum);
    size_t i = 0;
    for (auto const& choice : choices_vec) {
        u64 weight = static_cast<u64>(choice.weight);
        if (weight >= target) {
            return choice.value;
        }
        target -= weight;
        ++i;
    }
    return choices_vec[i - 1].value;
}

// An unsigned integer generator in the full u64 range.
//
// Prefers 8bit numbers, then 4bit, 16bit, 32bit and 64bit ones.
// Around 11% of the time it tries edge cases like 0 and various NumericLimits::max().
//
// Gen::number_u64() -> value 3,     RandomRun [0,3]
//                   -> value 8,     RandomRun [1,8]
//                   -> value 100,   RandomRun [2,100]
//                   -> value 5,     RandomRun [3,5]
//                   -> value 255,   RandomRun [4,1]
//                   -> value 65535, RandomRun [4,2]
//                   etc.
//
// Shrinks towards 0.
inline u64 number_u64()
{
    u64 bits = frequency(
        // weight, bits
        Choice { 4, 4 },
        Choice { 8, 8 },
        Choice { 2, 16 },
        Choice { 1, 32 },
        Choice { 1, 64 },
        Choice { 2, 0 });

    // The special cases go last as they can be the most extreme (large) values.

    if (bits == 0) {
        // Special cases, eg. max integers for u8, u16, u32, u64.
        return one_of(
            0U,
            NumericLimits<u8>::max(),
            NumericLimits<u16>::max(),
            NumericLimits<u32>::max(),
            NumericLimits<u64>::max());
    }

    u64 max = bits == 64
        ? NumericLimits<u64>::max()
        : ((u64)1 << bits) - 1;
    return number_u64(max);
}

// A generator returning `true` with the given `probability` (0..1).
//
// If probability <= 0, doesn't use any randomness and returns false.
// If probability >= 1, doesn't use any randomness and returns true.
//
// In general case:
// Gen::weighted_boolean(0.75)
//   -> value false, RandomRun [0]
//   -> value true,  RandomRun [1]
//
// Shrinks towards false.
inline bool weighted_boolean(f64 probability)
{
    if (probability <= 0)
        return false;
    if (probability >= 1)
        return true;

    u64 random_int = Test::randomness_source().draw_value(1, [&]() {
        f64 drawn_probability = get_random_probability();
        return drawn_probability <= probability ? 1 : 0;
    });
    bool random_bool = random_int == 1;
    return random_bool;
}

// A (fair) boolean generator.
//
// Gen::boolean()
//   -> value false, RandomRun [0]
//   -> value true,  RandomRun [1]
//
// Shrinks towards false.
inline bool boolean()
{
    return weighted_boolean(0.5);
}

// A vector generator of a random length between the given limits.
//
// Gen::vector(2,3,[]() { return Gen::number_u64(5); })
//   -> value [1,5],      RandomRun [1,1,1,5,0]
//   -> value [1,5,0],    RandomRun [1,1,1,5,1,0,0]
//   etc.
//
// In case `min == max`, the RandomRun footprint will be smaller, as there will
// be no randomness involved in figuring out the length:
//
// Gen::vector(3,3,[]() { return Gen::number_u64(5); })
//   -> value [1,3], RandomRun [1,3]
//   -> value [5,2], RandomRun [5,2]
//   etc.
//
// Shrinks towards shorter vectors, with simpler elements inside.
template<typename Fn>
inline Vector<InvokeResult<Fn>> vector(size_t min, size_t max, Fn item_gen)
{
    VERIFY(max >= min);

    size_t size = 0;
    Vector<InvokeResult<Fn>> acc;

    // Special case: no randomness for the boolean
    if (min == max) {
        while (size < min) {
            acc.append(item_gen());
            ++size;
        }
        return acc;
    }

    // General case: before each item we "flip a coin" to decide whether to
    // generate another one.
    //
    // This algorithm is used instead of the more intuitive "generate length,
    // then generate that many items" algorithm, because it produces RandomRun
    // patterns that shrink more easily.
    //
    // See the Hypothesis paper [1], section 3.3, around the paragraph starting
    // with "More commonly".
    //
    // [1]: https://drops.dagstuhl.de/opus/volltexte/2020/13170/pdf/LIPIcs-ECOOP-2020-13.pdf
    while (size < min) {
        acc.append(item_gen());
        ++size;
    }

    f64 average = static_cast<f64>(min + max) / 2.0;
    VERIFY(average > 0);

    // A geometric distribution: https://en.wikipedia.org/wiki/Geometric_distribution#Moments_and_cumulants
    // The below derives from the E(X) = 1/p formula.
    //
    // We need to flip the `p` to `1-p` as our success ("another item!") is
    // a "failure" in the geometric distribution's interpretation ("we fail X
    // times before succeeding the first time").
    //
    // That gives us `1 - 1/p`. Then, E(X) also contains the final success, so we
    // need to say `1 + average` instead of `average`, as it will mean "our X
    // items + the final failure that stops the process".
    f64 probability = 1.0 - 1.0 / (1.0 + average);

    while (size < max) {
        if (weighted_boolean(probability)) {
            acc.append(item_gen());
            ++size;
        } else {
            break;
        }
    }

    return acc;
}

// A vector generator of a given length.
//
// Gen::vector_of_length(3,[]() { return Gen::number_u64(5); })
//   -> value [1,5,0],    RandomRun [1,1,1,5,1,0,0]
//   -> value [2,9,3],    RandomRun [1,2,1,9,1,3,0]
//   etc.
//
// Shrinks towards shorter vectors, with simpler elements inside.
template<typename Fn>
inline Vector<InvokeResult<Fn>> vector(size_t length, Fn item_gen)
{
    return vector(length, length, item_gen);
}

// A vector generator of a random length between 0 and 32 elements.
//
// If you need a different length, use vector(max,item_gen) or
// vector(min,max,item_gen).
//
// Gen::vector([]() { return Gen::number_u64(5); })
//   -> value [],         RandomRun [0]
//   -> value [1],        RandomRun [1,1,0]
//   -> value [1,5],      RandomRun [1,1,1,5,0]
//   -> value [1,5,0],    RandomRun [1,1,1,5,1,0,0]
//   -> value [1,5,0,2],  RandomRun [1,1,1,5,1,0,1,2,0]
//   etc.
//
// Shrinks towards shorter vectors, with simpler elements inside.
template<typename Fn>
inline Vector<InvokeResult<Fn>> vector(Fn item_gen)
{
    return vector(0, 32, item_gen);
}

// A double generator in the [0,1) range.
//
// RandomRun footprint: a single number.
//
// Shrinks towards 0.
//
// Based on: https://dotat.at/@/2023-06-23-random-double.html
inline f64 percentage()
{
    return static_cast<f64>(number_u64() >> 11) * 0x1.0p-53;
}

// An internal double generator. This one won't make any attempt to shrink nicely.
// Test writers should use number_f64(f64 min, f64 max) instead.
inline f64 number_f64_scaled(f64 min, f64 max)
{
    VERIFY(max >= min);

    if (min == max)
        return min;

    f64 p = percentage();
    return min * (1.0 - p) + max * p;
}

inline f64 number_f64(f64 min, f64 max)
{
    // FIXME: after we figure out how to use frequency() with lambdas,
    // do edge cases and nicely shrinking float generators here

    return number_f64_scaled(min, max);
}

inline f64 number_f64()
{
    // FIXME: this could be much nicer to the user, at the expense of code complexity
    // We could follow Hypothesis' lead and remap integers 0..MAXINT to _simple_
    // floats rather than small floats. Meaning, we would like to prefer integers
    // over floats with decimal digits, positive numbers over negative numbers etc.
    // As a result, users would get failures with floats like 0, 1, or 0.5 instead of
    // ones like 1.175494e-38.
    // Check the doc comment in Hypothesis: https://github.com/HypothesisWorks/hypothesis/blob/master/hypothesis-python/src/hypothesis/internal/conjecture/floats.py

    return number_f64(NumericLimits<f64>::lowest(), NumericLimits<f64>::max());
}

// A double generator.
//
// The minimum value will always be NumericLimits<f64>::lowest().
// The maximum value is given by user in the argument.
//
// Prefers positive numbers, then negative numbers, then edge cases.
//
// Shrinks towards 0.
inline f64 number_f64(f64 max)
{
    // FIXME: after we figure out how to use frequency() with lambdas,
    // do edge cases and nicely shrinking float generators here

    return number_f64_scaled(NumericLimits<f64>::lowest(), max);
}

// TODO
inline u32 number_u32(u32 max)
{
    if (max == 0)
        return 0;

    u32 random = Test::randomness_source().draw_value(max, [&]() {
        // `clamp` to guard against integer overflow
        u32 exclusive_bound = AK::clamp(max + 1, max, NumericLimits<u32>::max());
        return AK::get_random_uniform(exclusive_bound);
    });
    return random;
}

// TODO
inline u32 number_u32(u32 min, u32 max)
{
    VERIFY(max >= min);
    return number_u32(max - min) + min;
}

// TODO
inline u32 number_u32()
{
    u32 bits = frequency(
        // weight, bits
        Choice { 4, 4 },
        Choice { 8, 8 },
        Choice { 2, 16 },
        Choice { 1, 32 },
        Choice { 1, 64 },
        Choice { 2, 0 });

    // The special cases go last as they can be the most extreme (large) values.

    if (bits == 0) {
        // Special cases, eg. max integers for u8, u16, u32.
        return one_of(
            0U,
            NumericLimits<u8>::max(),
            NumericLimits<u16>::max(),
            NumericLimits<u32>::max());
    }

    u32 max = bits == 32
        ? NumericLimits<u32>::max()
        : ((u32)1 << bits) - 1;
    return number_u32(max);
}

} // namespace Gen
} // namespace Randomized
} // namespace Test
