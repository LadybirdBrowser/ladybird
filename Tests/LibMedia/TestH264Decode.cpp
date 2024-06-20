/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>

#include "TestMediaCommon.h"

static NonnullOwnPtr<Media::VideoDecoder> make_decoder(Media::Matroska::SampleIterator const& iterator)
{
    return MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(Media::CodecID::H264, iterator.track().codec_private_data()));
}

TEST_CASE(avc_in_matroska)
{
    decode_video("./avc_in_matroska.mkv"sv, 50, make_decoder);
}
