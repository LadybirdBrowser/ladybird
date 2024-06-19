/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Video/VP9/Decoder.h>

#include "TestMediaCommon.h"

static NonnullOwnPtr<Media::VideoDecoder> make_decoder(Media::Matroska::SampleIterator const&)
{
    return make<Media::Video::VP9::Decoder>();
}

TEST_CASE(webm_in_vp9)
{
    decode_video("./vp9_in_webm.webm"sv, 25, make_decoder);
}

TEST_CASE(vp9_oob_blocks)
{
    decode_video("./vp9_oob_blocks.webm"sv, 240, make_decoder);
}

TEST_CASE(vp9_malformed_frame)
{
    Array test_inputs = {
        "./oss-fuzz-testcase-52630.vp9"sv,
        "./oss-fuzz-testcase-53977.vp9"sv,
        "./oss-fuzz-testcase-62054.vp9"sv,
        "./oss-fuzz-testcase-63182.vp9"sv
    };

    for (auto test_input : test_inputs) {
        auto file = MUST(Core::MappedFile::map(test_input));
        Media::Video::VP9::Decoder vp9_decoder;
        auto maybe_decoder_error = vp9_decoder.receive_sample(Duration::zero(), file->bytes());
        EXPECT(maybe_decoder_error.is_error());
    }
}

BENCHMARK_CASE(vp9_4k)
{
    decode_video("./vp9_4k.webm"sv, 2, make_decoder);
}

BENCHMARK_CASE(vp9_clamp_reference_mvs)
{
    decode_video("./vp9_clamp_reference_mvs.webm"sv, 92, make_decoder);
}
