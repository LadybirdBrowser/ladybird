/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Math.h>

namespace Audio {
using AK::Exponentials::exp;
using AK::Exponentials::log;
// Constants for logarithmic volume. See Sample::linear_to_log
// Corresponds to 60dB
constexpr float DYNAMIC_RANGE = 1000;
constexpr float VOLUME_A = 1 / DYNAMIC_RANGE;
float const VOLUME_B = log(DYNAMIC_RANGE);

// A single sample in an audio buffer.
// Values are floating point, and should range from -1.0 to +1.0
struct Sample {
    constexpr Sample() = default;

    // For mono
    constexpr explicit Sample(float left)
        : left(left)
        , right(left)
    {
    }

    // For stereo
    constexpr Sample(float left, float right)
        : left(left)
        , right(right)
    {
    }

    // Returns the absolute maximum range (separate per channel) of the given sample buffer.
    // For example { 0.8, 0 } means that samples on the left channel occupy the range { -0.8, 0.8 },
    // while all samples on the right channel are 0.
    static Sample max_range(ReadonlySpan<Sample> span)
    {
        Sample result { NumericLimits<float>::min_normal(), NumericLimits<float>::min_normal() };
        for (Sample sample : span) {
            result.left = max(result.left, AK::fabs(sample.left));
            result.right = max(result.right, AK::fabs(sample.right));
        }
        return result;
    }

    void clip()
    {
        if (left > 1)
            left = 1;
        else if (left < -1)
            left = -1;

        if (right > 1)
            right = 1;
        else if (right < -1)
            right = -1;
    }

    // Logarithmic scaling, as audio should ALWAYS do.
    // Reference: https://www.dr-lex.be/info-stuff/volumecontrols.html
    // We use the curve `factor = a * exp(b * change)`,
    // where change is the input fraction we want to change by,
    // a = 1/1000, b = ln(1000) = 6.908 and factor is the multiplier used.
    // The value 1000 represents the dynamic range in sound pressure, which corresponds to 60 dB(A).
    // This is a good dynamic range because it can represent all loudness values from
    // 30 dB(A) (barely hearable with background noise)
    // to 90 dB(A) (almost too loud to hear and about the reasonable limit of actual sound equipment).
    //
    // Format ranges:
    // - Linear:        0.0 to 1.0
    // - Logarithmic:   0.0 to 1.0

    ALWAYS_INLINE float linear_to_log(float const change) const
    {
        // TODO: Add linear slope around 0
        return VOLUME_A * exp(VOLUME_B * change);
    }

    ALWAYS_INLINE float log_to_linear(float const val) const
    {
        // TODO: Add linear slope around 0
        return log(val / VOLUME_A) / VOLUME_B;
    }

    ALWAYS_INLINE Sample& log_multiply(float const change)
    {
        float factor = linear_to_log(change);
        left *= factor;
        right *= factor;
        return *this;
    }

    ALWAYS_INLINE Sample log_multiplied(float const volume_change) const
    {
        Sample new_frame { left, right };
        new_frame.log_multiply(volume_change);
        return new_frame;
    }

    // Constant power panning
    ALWAYS_INLINE Sample& pan(float const position)
    {
        float const pi_over_2 = AK::Pi<float> * 0.5f;
        float const root_over_2 = AK::sqrt<float>(2.0) * 0.5f;
        float const angle = position * pi_over_2 * 0.5f;
        float s, c;
        AK::sincos<float>(angle, s, c);
        left *= root_over_2 * (c - s);
        right *= root_over_2 * (c + s);
        return *this;
    }

    ALWAYS_INLINE Sample panned(float const position) const
    {
        Sample new_sample { left, right };
        new_sample.pan(position);
        return new_sample;
    }

    constexpr Sample& operator*=(float const mult)
    {
        left *= mult;
        right *= mult;
        return *this;
    }

    constexpr Sample operator*(float const mult) const
    {
        return { left * mult, right * mult };
    }

    constexpr Sample& operator+=(Sample const& other)
    {
        left += other.left;
        right += other.right;
        return *this;
    }
    constexpr Sample& operator+=(float other)
    {
        left += other;
        right += other;
        return *this;
    }

    constexpr Sample operator+(Sample const& other) const
    {
        return { left + other.left, right + other.right };
    }

    float left { 0 };
    float right { 0 };
};

}

namespace AK {

template<>
struct Formatter<Audio::Sample> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Audio::Sample const& value)
    {
        return Formatter<FormatString>::format(builder, "[{}, {}]"sv, value.left, value.right);
    }
};

}
