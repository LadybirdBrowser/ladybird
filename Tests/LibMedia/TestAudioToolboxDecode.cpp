/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/File.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/AudioToolbox/AudioToolboxAudioDecoder.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/Track.h>
#include <LibTest/TestCase.h>

namespace {

// Matroska stores timestamps in milliseconds, so output timed from them can land up to one away from exact.
constexpr auto TIMESTAMP_TOLERANCE = AK::Duration::from_milliseconds(1);

struct CodedStream {
    Media::Track track;
    Vector<Media::CodedFrame> samples;
};

struct DecodedBlock {
    Audio::SampleSpecification sample_specification;
    AK::Duration start;
    AK::Duration end;
    size_t frame_count { 0 };
};

CodedStream read_coded_stream(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
    VERIFY(track.has_value());
    MUST(demuxer->create_context_for_track(*track));

    Vector<Media::CodedFrame> samples;
    while (true) {
        auto sample = demuxer->get_next_sample_for_track(*track);
        if (sample.is_error()) {
            VERIFY(sample.error().category() == Media::DecoderErrorCategory::EndOfStream);
            break;
        }
        samples.append(sample.release_value());
    }
    return { track.release_value(), move(samples) };
}

NonnullOwnPtr<Media::AudioToolbox::AudioToolboxAudioDecoder> create_decoder(Media::Track const& track)
{
    return MUST(Media::AudioToolbox::AudioToolboxAudioDecoder::try_create(Media::CodecID::AAC, track.audio_data().sample_specification, {}));
}

// Returns the error category that stopped the decoder.
Media::DecoderErrorCategory write_available_blocks(Media::AudioDecoder& decoder, Vector<DecodedBlock>& blocks, Vector<Vector<float>>* channel_samples = nullptr)
{
    Media::AudioBlock block;
    while (true) {
        auto result = decoder.write_next_block(block);
        if (result.is_error())
            return result.error().category();

        blocks.append({ block.sample_specification(), block.media_time_start(), block.media_time_end(), block.frame_count() });
        if (channel_samples == nullptr)
            continue;
        channel_samples->resize(block.channel_count());
        for (u8 channel = 0; channel < block.channel_count(); channel++)
            channel_samples->at(channel).append(block.channel_data(channel).data(), block.frame_count());
    }
}

Vector<DecodedBlock> decode_entire_stream(Media::AudioDecoder& decoder, CodedStream const& stream, Vector<Vector<float>>* channel_samples = nullptr)
{
    Vector<DecodedBlock> blocks;
    for (auto const& sample : stream.samples) {
        MUST(decoder.receive_coded_data(sample));
        EXPECT_EQ(write_available_blocks(decoder, blocks, channel_samples), Media::DecoderErrorCategory::NeedsMoreInput);
    }
    decoder.signal_end_of_stream();
    EXPECT_EQ(write_available_blocks(decoder, blocks, channel_samples), Media::DecoderErrorCategory::EndOfStream);
    return blocks;
}

bool is_within_tolerance(AK::Duration actual, AK::Duration expected)
{
    return actual >= expected - TIMESTAMP_TOLERANCE && actual <= expected + TIMESTAMP_TOLERANCE;
}

void expect_contiguous_output(Vector<DecodedBlock> const& blocks, AK::Duration start)
{
    EXPECT(!blocks.is_empty());
    auto expected_start = start;
    for (auto const& block : blocks) {
        EXPECT(is_within_tolerance(block.start, expected_start));
        expected_start = block.end;
    }
}

// Each channel of the surround source is a tone of a different frequency.
Optional<u32> source_frequency_for_channel(Audio::Channel channel)
{
    switch (channel) {
    case Audio::Channel::FrontLeft:
        return 400;
    case Audio::Channel::FrontRight:
        return 800;
    case Audio::Channel::FrontCenter:
        return 200;
    case Audio::Channel::LowFrequency:
        return 100;
    case Audio::Channel::BackLeft:
        return 3200;
    case Audio::Channel::BackRight:
        return 6400;
    default:
        return {};
    }
}

size_t total_frame_count(Vector<DecodedBlock> const& blocks)
{
    size_t frame_count = 0;
    for (auto const& block : blocks)
        frame_count += block.frame_count;
    return frame_count;
}

}

TEST_CASE(decodes_aac_lc)
{
    auto stream = read_coded_stream("./aac_lc_in_matroska.mka"sv);
    auto decoder = create_decoder(stream.track);

    auto blocks = decode_entire_stream(*decoder, stream);

    for (auto const& block : blocks)
        EXPECT_EQ(block.sample_specification, Audio::SampleSpecification(48'000, Audio::ChannelMap::stereo()));
    expect_contiguous_output(blocks, stream.samples.first().presentation_timestamp());
    EXPECT_EQ(total_frame_count(blocks), stream.samples.size() * 1024);
}

TEST_CASE(he_aac_output_stays_contiguous_across_the_frames_that_sbr_delays)
{
    auto stream = read_coded_stream("./he_aac_in_matroska.mka"sv);
    auto decoder = create_decoder(stream.track);

    auto blocks = decode_entire_stream(*decoder, stream);

    // SBR doubles the core coder's sample rate and frame count.
    for (auto const& block : blocks)
        EXPECT_EQ(block.sample_specification, Audio::SampleSpecification(48'000, Audio::ChannelMap::stereo()));
    expect_contiguous_output(blocks, stream.samples.first().presentation_timestamp());
    EXPECT_EQ(total_frame_count(blocks), stream.samples.size() * 2048);
}

TEST_CASE(flushing_restarts_output_at_the_next_packet)
{
    auto stream = read_coded_stream("./he_aac_in_matroska.mka"sv);
    auto decoder = create_decoder(stream.track);

    Vector<DecodedBlock> blocks;
    for (size_t index = 0; index < 4; index++) {
        TRY_OR_FAIL(decoder->receive_coded_data(stream.samples[index]));
        EXPECT_EQ(write_available_blocks(*decoder, blocks), Media::DecoderErrorCategory::NeedsMoreInput);
    }

    decoder->flush();
    blocks.clear();

    auto const& resumed_sample = stream.samples[8];
    TRY_OR_FAIL(decoder->receive_coded_data(resumed_sample));
    EXPECT_EQ(write_available_blocks(*decoder, blocks), Media::DecoderErrorCategory::NeedsMoreInput);
    expect_contiguous_output(blocks, resumed_sample.presentation_timestamp());
}

TEST_CASE(a_new_configuration_takes_over_once_the_previous_output_is_written)
{
    static constexpr size_t he_aac_sample_count = 4;

    auto he_aac_stream = read_coded_stream("./he_aac_in_matroska.mka"sv);
    auto lc_stream = read_coded_stream("./aac_lc_in_matroska.mka"sv);
    auto decoder = create_decoder(he_aac_stream.track);

    Vector<DecodedBlock> blocks;
    for (size_t index = 0; index < he_aac_sample_count; index++) {
        TRY_OR_FAIL(decoder->receive_coded_data(he_aac_stream.samples[index]));
        EXPECT_EQ(write_available_blocks(*decoder, blocks), Media::DecoderErrorCategory::NeedsMoreInput);
    }

    auto lc_sample = lc_stream.samples.first();
    EXPECT(lc_sample.new_codec_configuration().has_value());
    lc_sample.set_presentation_timestamp(he_aac_stream.samples[he_aac_sample_count].presentation_timestamp());
    TRY_OR_FAIL(decoder->receive_coded_data(lc_sample));
    EXPECT_EQ(write_available_blocks(*decoder, blocks), Media::DecoderErrorCategory::NeedsMoreInput);

    // The frames that the HE-AAC decoder delayed are written before the LC decoder's first packet.
    expect_contiguous_output(blocks, he_aac_stream.samples.first().presentation_timestamp());
    EXPECT_EQ(total_frame_count(blocks), (he_aac_sample_count * 2048) + 1024);
}

TEST_CASE(surround_channels_are_written_in_the_order_of_their_channel_map)
{
    auto stream = read_coded_stream("./aac_5_1_in_matroska.mka"sv);
    auto decoder = create_decoder(stream.track);

    Vector<Vector<float>> channel_samples;
    auto blocks = decode_entire_stream(*decoder, stream, &channel_samples);
    auto const& channel_map = blocks.first().sample_specification.channel_map();
    EXPECT_EQ(channel_map.channel_count(), 6);

    // Skip the encoder's priming, and count zero crossings for a quarter second.
    static constexpr size_t first_frame = 4096;
    static constexpr size_t frame_count = 12'000;
    for (u8 channel = 0; channel < channel_map.channel_count(); channel++) {
        auto expected_frequency = source_frequency_for_channel(channel_map.channel_at(channel));
        EXPECT(expected_frequency.has_value());

        auto samples = channel_samples[channel].span().slice(first_frame, frame_count);
        size_t zero_crossing_count = 0;
        for (size_t index = 1; index < samples.size(); index++) {
            if ((samples[index - 1] < 0) != (samples[index] < 0))
                zero_crossing_count++;
        }
        auto measured_frequency = static_cast<double>(zero_crossing_count) / 2 * 48'000 / frame_count;
        EXPECT_APPROXIMATE_WITH_ERROR(measured_frequency, static_cast<double>(expected_frequency.value()), expected_frequency.value() * 0.05);
    }
}

TEST_CASE(aac_without_a_configuration_is_left_to_another_decoder)
{
    auto decoder = MUST(Media::AudioToolbox::AudioToolboxAudioDecoder::try_create(Media::CodecID::AAC, Audio::SampleSpecification { 48'000, Audio::ChannelMap::stereo() }, {}));

    Media::CodedFrame frame { Media::CodecID::AAC, AK::Duration::zero(), AK::Duration::zero(), AK::Duration::zero(), Media::FrameFlags::Keyframe, MUST(FixedArray<u8>::create({ 0x21, 0x10, 0x04 })), FixedArray<u8> {} };
    auto result = decoder->receive_coded_data(frame);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().category(), Media::DecoderErrorCategory::NotImplemented);
}
