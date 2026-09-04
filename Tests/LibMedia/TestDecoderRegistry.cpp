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
    auto audio_selection = Media::select_audio_decoder(Media::ParsedCodec { Media::CodecID::S16LE });
    EXPECT(audio_selection.has_value());
    auto audio_decoder = Media::create_audio_decoder(audio_selection, Media::CodecID::S16LE, Audio::SampleSpecification { 8'000, Audio::ChannelMap::mono() }, {});
    EXPECT(!audio_decoder.is_error());

    auto video_selection = Media::select_video_decoder(Media::ParsedCodec { Media::CodecID::H264 });
    EXPECT(video_selection.has_value());
    auto video_decoder = Media::create_video_decoder(video_selection, Media::CodecID::H264, {});
    EXPECT(!video_decoder.is_error());
}

TEST_CASE(selecting_past_the_last_decoder_finds_nothing)
{
    auto codec = Media::ParsedCodec { Media::CodecID::H264 };

    auto selection = Media::select_video_decoder(codec);
    EXPECT(selection.has_value());

    // Every decoder below the last one that claimed the stream has been exhausted.
    auto last = selection;
    while (last.has_value())
        last = Media::select_video_decoder(codec, last);
    EXPECT(!last.has_value());
}

TEST_CASE(an_audio_codec_selects_no_video_decoder)
{
    EXPECT(!Media::select_video_decoder(Media::ParsedCodec { Media::CodecID::Vorbis }).has_value());
}

TEST_CASE(selecting_past_the_last_audio_decoder_finds_nothing)
{
    auto codec = Media::ParsedCodec { Media::CodecID::Vorbis };

    auto selection = Media::select_audio_decoder(codec);
    EXPECT(selection.has_value());

    auto last = selection;
    while (last.has_value())
        last = Media::select_audio_decoder(codec, last);
    EXPECT(!last.has_value());
}

TEST_CASE(a_video_codec_selects_no_audio_decoder)
{
    EXPECT(!Media::select_audio_decoder(Media::ParsedCodec { Media::CodecID::VP9 }).has_value());
}
