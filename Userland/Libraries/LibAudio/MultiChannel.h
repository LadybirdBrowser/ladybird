/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/FixedArray.h>
#include <LibAudio/Sample.h>

namespace Audio {

// Downmixes any number of channels to stereo, under the assumption that standard channel layout is followed:
// 1 channel = mono
// 2 channels = stereo (left, right)
// 3 channels = left, right, center
// 4 channels = front left/right, back left/right
// 5 channels = front left/right, center, back left/right
// 6 channels = front left/right, center, LFE, back left/right
// 7 channels = front left/right, center, LFE, back center, side left/right
// 8 channels = front left/right, center, LFE, back left/right, side left/right
// Additionally, performs sample rescaling to go from integer samples to floating-point samples.
template<ArrayLike<i64> ChannelType, ArrayLike<ChannelType> InputType>
ErrorOr<FixedArray<Sample>> downmix_surround_to_stereo(InputType const& input, float sample_scale_factor)
{
    if (input.size() == 0)
        return Error::from_string_literal("Cannot resample from 0 channels");

    auto channel_count = input.size();
    auto sample_count = input[0].size();

    FixedArray<Sample> output = TRY(FixedArray<Sample>::create(sample_count));

    // FIXME: We could figure out a better way to mix the channels, possibly spatially, but for now:
    //        - Center and LFE channels are added to both left and right.
    //        - All left channels are added together on the left, all right channels are added together on the right.
    switch (channel_count) {
    case 1:
        for (auto i = 0u; i < sample_count; ++i)
            output[i] = Sample { input[0][i] * sample_scale_factor };
        break;
    case 2:
        for (auto i = 0u; i < sample_count; ++i)
            output[i] = Sample {
                input[0][i] * sample_scale_factor,
                input[1][i] * sample_scale_factor
            };
        break;
    case 3:
        for (auto i = 0u; i < sample_count; ++i)
            output[i] = Sample {
                input[0][i] * sample_scale_factor + input[2][i] * sample_scale_factor,
                input[1][i] * sample_scale_factor + input[2][i] * sample_scale_factor
            };
        break;
    case 4:
        for (auto i = 0u; i < sample_count; ++i)
            output[i] = Sample {
                input[0][i] * sample_scale_factor + input[2][i] * sample_scale_factor,
                input[1][i] * sample_scale_factor + input[3][i] * sample_scale_factor
            };
        break;
    case 5:
        for (auto i = 0u; i < sample_count; ++i)
            output[i] = Sample {
                input[0][i] * sample_scale_factor + input[3][i] * sample_scale_factor + input[2][i] * sample_scale_factor,
                input[1][i] * sample_scale_factor + input[4][i] * sample_scale_factor + input[2][i] * sample_scale_factor
            };
        break;
    case 6:
        for (auto i = 0u; i < sample_count; ++i) {
            output[i] = Sample {
                input[0][i] * sample_scale_factor + input[4][i] * sample_scale_factor + input[2][i] * sample_scale_factor + input[3][i] * sample_scale_factor,
                input[1][i] * sample_scale_factor + input[5][i] * sample_scale_factor + input[2][i] * sample_scale_factor + input[3][i] * sample_scale_factor
            };
        }
        break;
    case 7:
        for (auto i = 0u; i < sample_count; ++i) {
            output[i] = Sample {
                input[0][i] * sample_scale_factor + input[5][i] * sample_scale_factor + input[2][i] * sample_scale_factor + input[3][i] * sample_scale_factor + input[4][i] * sample_scale_factor,
                input[1][i] * sample_scale_factor + input[6][i] * sample_scale_factor + input[2][i] * sample_scale_factor + input[3][i] * sample_scale_factor + input[4][i] * sample_scale_factor
            };
        }
        break;
    case 8:
        for (auto i = 0u; i < sample_count; ++i) {
            output[i] = Sample {
                input[0][i] * sample_scale_factor + input[4][i] * sample_scale_factor + input[6][i] * sample_scale_factor + input[2][i] * sample_scale_factor + input[3][i] * sample_scale_factor,
                input[1][i] * sample_scale_factor + input[5][i] * sample_scale_factor + input[7][i] * sample_scale_factor + input[2][i] * sample_scale_factor + input[3][i] * sample_scale_factor
            };
        }
        break;
    default:
        return Error::from_string_literal("Invalid number of channels greater than 8");
    }

    return output;
}

}
