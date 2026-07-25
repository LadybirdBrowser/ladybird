/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Tests/LibMedia/TestMediaCommon.h>

TEST_CASE(xiph_lacing)
{
    // FIXME: 530 samples are marked to be discarded by the DiscardPadding element.
    decode_audio("test-webm-xiph-lacing.mka"sv, 48'000, 2, 578038 + 530);
}

TEST_CASE(blocks_are_converted_to_the_output_specification)
{
    decode_audio("test-webm-xiph-lacing.mka"sv, 96'000, 2, (578038 + 530) * 2, {},
        Audio::SampleSpecification(96'000, Audio::ChannelMap::stereo()));
}
