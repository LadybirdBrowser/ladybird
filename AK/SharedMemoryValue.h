/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Types.h>

namespace AK {

// decode() must refuse every representation that the type's own code never produces.
template<>
struct SharedMemoryCodec<Duration> {
    struct Representation {
        i64 seconds { 0 };
        u32 nanoseconds { 0 };
    };

    static constexpr Representation encode(Duration value) { return { value.m_seconds, value.m_nanoseconds }; }
    static constexpr Optional<Duration> decode(Representation representation)
    {
        return Duration::maybe_from_seconds_and_nanoseconds(representation.seconds, representation.nanoseconds);
    }
};

template<>
struct SharedMemoryCodec<MonotonicTime> {
    using Representation = SharedMemoryCodec<Duration>::Representation;

    static constexpr Representation encode(MonotonicTime value) { return SharedMemoryCodec<Duration>::encode(value - MonotonicTime {}); }
    static constexpr Optional<MonotonicTime> decode(Representation representation)
    {
        auto offset = SharedMemoryCodec<Duration>::decode(representation);
        if (!offset.has_value())
            return {};
        return MonotonicTime {} + offset.value();
    }
};

// A representation its codec refuses reads as the type's default value.
template<typename T>
class SharedMemoryValue {
    using Codec = SharedMemoryCodec<T>;

public:
    constexpr SharedMemoryValue()
        : SharedMemoryValue(T {})
    {
    }

    constexpr SharedMemoryValue(T const& value)
        : m_representation(Codec::encode(value))
    {
    }

    constexpr T value() const { return Codec::decode(m_representation).value_or(T {}); }
    constexpr operator T() const { return value(); }

private:
    typename Codec::Representation m_representation;
};

}

#if USING_AK_GLOBALLY
using AK::SharedMemoryCodec;
using AK::SharedMemoryValue;
#endif
