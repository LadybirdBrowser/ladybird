/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DecoderRegistry.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/FFmpeg/FFmpegVideoDecoder.h>
#include <LibTest/TestCase.h>

#ifdef AK_OS_MACOS
#    include <LibMedia/AudioToolbox/AudioToolboxAudioDecoder.h>
#endif

TEST_CASE(ffmpeg_decoder_capabilities)
{
    auto ffmpeg_capabilities = [](Media::ParsedCodec const& codec) {
        if (Media::track_type_from_codec_id(codec.codec_id()) == Media::TrackType::Audio)
            return Media::FFmpeg::FFmpegAudioDecoder::capabilities(codec);
        return Media::FFmpeg::FFmpegVideoDecoder::capabilities(codec);
    };

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
        auto capabilities = ffmpeg_capabilities(Media::ParsedCodec { codec_id });
        EXPECT(capabilities.has_value());
        EXPECT(capabilities->smooth);
        EXPECT_EQ(capabilities->power_efficient, Media::track_type_from_codec_id(codec_id) == Media::TrackType::Audio);
    }
}

TEST_CASE(an_unknown_codec_has_no_capabilities)
{
    EXPECT(!Media::decoder_capabilities(Media::ParsedCodec { Media::CodecID::Unknown }).has_value());
}

TEST_CASE(decoder_creation)
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

#ifdef AK_OS_MACOS
TEST_CASE(audiotoolbox_decoder_capabilities)
{
    for (auto codec_string : { "mp4a.40.2"sv, "mp4a.40.5"sv, "mp4a.40.23"sv, "mp4a.40.29"sv, "mp4a.40.39"sv, "mp4a.67"sv }) {
        auto codec = Media::parse_codec_parameters_string(codec_string);
        VERIFY(codec.has_value());
        auto capabilities = Media::AudioToolbox::AudioToolboxAudioDecoder::capabilities(*codec);
        EXPECT(capabilities.has_value());
        EXPECT(capabilities->smooth);
        EXPECT(capabilities->power_efficient);
    }

    auto aac_main = Media::parse_codec_parameters_string("mp4a.40.1"sv);
    VERIFY(aac_main.has_value());
    EXPECT(!Media::AudioToolbox::AudioToolboxAudioDecoder::capabilities(*aac_main).has_value());

    EXPECT(!Media::AudioToolbox::AudioToolboxAudioDecoder::capabilities(Media::ParsedCodec { Media::CodecID::Vorbis }).has_value());
}

TEST_CASE(aac_selects_the_audiotoolbox_decoder_first)
{
    auto selection = Media::select_audio_decoder(Media::ParsedCodec { Media::CodecID::AAC });
    EXPECT(selection.has_value());
    auto decoder = TRY_OR_FAIL(Media::create_audio_decoder(selection, Media::CodecID::AAC, Audio::SampleSpecification { 48'000, Audio::ChannelMap::stereo() }, {}));
    EXPECT(is<Media::AudioToolbox::AudioToolboxAudioDecoder>(*decoder));
}
#endif
