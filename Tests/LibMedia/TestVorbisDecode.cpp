/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Tests/LibMedia/TestMediaCommon.h>

TEST_CASE(44_1Khz_stereo)
{
    // FIXME: 96 samples are marked to be discarded, but AudioDataProvider currently is not aware of this.
    decode_audio("vorbis/44_1Khz_stereo.ogg"sv, 44100, 2, 352800 + 96);
}
