/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DecoderRegistry.h>
#include <LibTest/TestCase.h>

TEST_CASE(ffmpeg_decoder_capabilities)
{
    for (auto codec_id : {
             Media::CodecID::VP8,
             Media::CodecID::VP9,
             Media::CodecID::H264,
             Media::CodecID::H265,
             Media::CodecID::MP3,
             Media::CodecID::AAC,
             Media::CodecID::AV1,
             Media::CodecID::Theora,
             Media::CodecID::Vorbis,
             Media::CodecID::Opus,
             Media::CodecID::FLAC,
             Media::CodecID::U8,
             Media::CodecID::S16LE,
             Media::CodecID::S24LE,
             Media::CodecID::S32LE,
             Media::CodecID::F32LE,
             Media::CodecID::ALaw,
             Media::CodecID::MuLaw,
         }) {
        auto capabilities = Media::decoder_capabilities(Media::ParsedCodec { codec_id });
        EXPECT(capabilities.has_value());
        EXPECT(capabilities->smooth);
        EXPECT(!capabilities->power_efficient);
    }

    EXPECT(!Media::decoder_capabilities(Media::ParsedCodec { Media::CodecID::Unknown }).has_value());
}

TEST_CASE(ffmpeg_decoder_creation)
{
    auto audio_decoder = Media::create_audio_decoder(Media::CodecID::S16LE, Audio::SampleSpecification { 8'000, Audio::ChannelMap::mono() }, {});
    EXPECT(!audio_decoder.is_error());

    auto video_decoder = Media::create_video_decoder(Media::CodecID::H264, {});
    EXPECT(!video_decoder.is_error());
}
