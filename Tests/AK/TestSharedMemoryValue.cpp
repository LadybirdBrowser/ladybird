/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/SharedMemoryValue.h>
#include <LibTest/TestCase.h>

template<typename T>
static SharedMemoryValue<T> forge(typename SharedMemoryCodec<T>::Representation representation)
{
    return bit_cast<SharedMemoryValue<T>>(representation);
}

TEST_CASE(durations_round_trip)
{
    SharedMemoryValue<AK::Duration> value = AK::Duration::from_milliseconds(-1500);
    EXPECT_EQ(value.value(), AK::Duration::from_milliseconds(-1500));
    EXPECT_EQ(static_cast<AK::Duration>(SharedMemoryValue<AK::Duration> { AK::Duration::max() }), AK::Duration::max());
}

TEST_CASE(non_canonical_durations_read_as_zero)
{
    EXPECT_EQ(forge<AK::Duration>({ 5, 999'999'999 }).value(), AK::Duration::from_nanoseconds(5'999'999'999));
    EXPECT_EQ(forge<AK::Duration>({ 5, 1'000'000'000 }).value(), AK::Duration::zero());
    EXPECT_EQ(forge<AK::Duration>({ 5, NumericLimits<u32>::max() }).value(), AK::Duration::zero());
}

TEST_CASE(monotonic_times_round_trip)
{
    auto now = MonotonicTime::now();
    SharedMemoryValue<MonotonicTime> value = now;
    EXPECT_EQ(value.value(), now);
    EXPECT_EQ(forge<MonotonicTime>({ 5, 1'000'000'000 }).value(), MonotonicTime {});
}
