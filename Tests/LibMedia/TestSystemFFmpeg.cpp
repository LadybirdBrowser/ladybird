/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMediaCodecs/AudioBlock.h>
#include <LibMediaCodecs/Codecs/AAC.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMediaCodecs/FFmpeg/SystemFFmpeg.h>

#include "TestMediaCommon.h"

// The bundled libavcodec stands in for a system one; the loader treats it like any other.
static NonnullOwnPtr<Media::FFmpeg::SystemFFmpeg> load_bundled_library()
{
    return MUST(Media::FFmpeg::SystemFFmpeg::try_load(LADYBIRD_BUNDLED_LIBAVCODEC_PATH ""sv));
}

TEST_CASE(loaded_library_reports_its_major_and_decoders)
{
    auto library = load_bundled_library();
    EXPECT(library->major() >= 60u);
    EXPECT(library->has_decoder(Media::CodecID::VP9));
    EXPECT(!library->provides_decoder_missing_from_bundle());
}

TEST_CASE(a_missing_library_is_an_error)
{
    auto result = Media::FFmpeg::SystemFFmpeg::try_load("libavcodec-that-does-not-exist.so"sv);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().category(), Media::DecoderErrorCategory::NotImplemented);
}

static size_t decode_every_frame(Media::FFmpeg::SystemFFmpeg const& library, StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).release_value();
    MUST(demuxer->create_context_for_track(track));

    auto first_sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT(Media::FFmpeg::FFmpegVideoDecoder::capabilities(library.functions(), Media::ParsedCodec { first_sample.codec_id() }).has_value());
    auto decoder = MUST(Media::FFmpeg::FFmpegVideoDecoder::try_create(library.functions(), first_sample.codec_id(), first_sample.new_codec_configuration().value_or({})));
    MUST(decoder->receive_coded_data(first_sample, Media::DecodeIntent::Output));

    size_t frame_count = 0;
    auto drain = [&] {
        while (true) {
            auto frame = decoder->take_next_output(track.video_data().cicp);
            if (frame.is_error())
                return frame.release_error();
            frame_count++;
        }
    };

    while (true) {
        auto sample = demuxer->get_next_sample_for_track(track);
        if (sample.is_error()) {
            EXPECT_EQ(sample.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        MUST(decoder->receive_coded_data(sample.release_value(), Media::DecodeIntent::Output));
        EXPECT_EQ(drain().category(), Media::DecoderErrorCategory::NeedsMoreInput);
    }
    decoder->signal_end_of_stream();
    EXPECT_EQ(drain().category(), Media::DecoderErrorCategory::EndOfStream);
    return frame_count;
}

TEST_CASE(decodes_through_the_loaded_library)
{
    auto library = load_bundled_library();
    EXPECT(decode_every_frame(*library, "./vp9_in_webm.webm"sv) > 0);
}

// HEVC sits after codecs that newer libavcodec majors removed, so its ID differs from the bundled headers'.
TEST_CASE(decodes_hevc_through_the_system_library)
{
    auto const* library = Media::FFmpeg::SystemFFmpeg::the();
    if (library == nullptr || !library->has_decoder(Media::CodecID::H265)) {
        warnln("No system FFmpeg decodes HEVC, skipping");
        return;
    }
    EXPECT(decode_every_frame(*library, "./hevc_open_gop.mp4"sv) > 0);
}

enum class ConfigurationPlacement {
    Decoder,
    FirstFrame,
};

static Vector<float> decode_aac_lc_with_configuration(Media::FFmpeg::SystemFFmpeg const& library, ReadonlyBytes configuration, ConfigurationPlacement placement)
{
    auto file = MUST(Core::File::open("./aac_lc_in_matroska.mka"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Audio)).release_value();
    MUST(demuxer->create_context_for_track(track));

    ReadonlyBytes decoder_configuration;
    if (placement == ConfigurationPlacement::Decoder)
        decoder_configuration = configuration;
    auto decoder = MUST(Media::FFmpeg::FFmpegAudioDecoder::try_create(library.functions(), Media::CodecID::AAC, track.audio_data().sample_specification, decoder_configuration));

    Vector<float> samples;
    auto drain = [&] {
        Media::AudioBlock block;
        while (true) {
            auto result = decoder->write_next_block(block);
            if (result.is_error())
                return result.release_error();
            for (size_t frame = 0; frame < block.frame_count(); frame++) {
                for (u8 channel = 0; channel < block.channel_count(); channel++)
                    samples.append(block.channel_data(channel)[frame]);
            }
        }
    };

    bool is_first_frame = true;
    while (true) {
        auto sample = demuxer->get_next_sample_for_track(track);
        if (sample.is_error()) {
            EXPECT_EQ(sample.error().category(), Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        // Rebuild each frame so that only the configuration under test reaches the decoder.
        Media::CodedFrame frame { sample.value().codec_id(), sample.value().presentation_timestamp(), sample.value().decode_timestamp(), sample.value().duration(), sample.value().flags(), MUST(FixedArray<u8>::create(sample.value().data())) };
        if (placement == ConfigurationPlacement::FirstFrame && is_first_frame)
            frame.set_new_codec_configuration(MUST(FixedArray<u8>::create(configuration)));
        is_first_frame = false;
        MUST(decoder->receive_coded_data(frame));
        EXPECT_EQ(drain().category(), Media::DecoderErrorCategory::NeedsMoreInput);
    }
    decoder->signal_end_of_stream();
    EXPECT_EQ(drain().category(), Media::DecoderErrorCategory::EndOfStream);
    return samples;
}

// hls.js 0.12.4 declares SBR at the core's own rate over AAC-LC, which FFmpeg decodes into a distorted core.
TEST_CASE(aac_with_spectral_band_replication_at_the_core_sample_rate_decodes_as_its_core)
{
    auto const* library = Media::FFmpeg::SystemFFmpeg::the();
    if (library == nullptr || !library->has_decoder(Media::CodecID::AAC)) {
        warnln("No system FFmpeg decodes AAC, skipping");
        return;
    }

    // AAC-LC at 48000 Hz, stereo.
    Array<u8, 2> low_complexity { 0x11, 0x90 };
    // The same core under hierarchical SBR signaling at 48000 Hz.
    Array<u8, 4> hierarchical { 0x29, 0x91, 0x88, 0x00 };
    auto backward_compatible = TRY_OR_FAIL(Media::Codecs::AAC::create_configuration_record(Media::Codecs::AAC::LOW_COMPLEXITY_AUDIO_OBJECT_TYPE, 48000, 2, 48000));

    auto expected = decode_aac_lc_with_configuration(*library, low_complexity, ConfigurationPlacement::Decoder);
    EXPECT(!expected.is_empty());
    for (auto placement : { ConfigurationPlacement::Decoder, ConfigurationPlacement::FirstFrame }) {
        EXPECT_EQ(decode_aac_lc_with_configuration(*library, hierarchical, placement), expected);
        EXPECT_EQ(decode_aac_lc_with_configuration(*library, backward_compatible.span(), placement), expected);
    }
}
