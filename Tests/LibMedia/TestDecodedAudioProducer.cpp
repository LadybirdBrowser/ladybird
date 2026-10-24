/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibMedia/DecoderRegistrations.h>
#include <Tests/LibMedia/StateLosingAudioDecoder.h>
#include <Tests/LibMedia/TestMediaCommon.h>

TEST_SETUP
{
    Media::install_video_decoders({ Media::hardware_video_decoders(), Media::ffmpeg_video_decoders() });
    Media::install_audio_decoders({ StateLosingAudioDecoder::registrations(), Media::audio_decoders() });
}

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

TEST_CASE(converted_blocks_split_to_fit_the_block_capacity)
{
    // At 4x the sample rate, each decoded block's conversion exceeds a single block's frame capacity.
    decode_audio("test-webm-xiph-lacing.mka"sv, 192'000, 2, (578038 + 530) * 4, {},
        Audio::SampleSpecification(192'000, Audio::ChannelMap::stereo()));
}

static NonnullRefPtr<Media::DecodedAudioProducer> create_started_audio_producer(Core::EventLoop& loop, StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());
    auto producer = MUST(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0]));
    producer->set_error_handler([](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->start();
    return producer;
}

struct BlockFrameRange {
    i64 first_frame_index;
    i64 end_frame_index;
};

static Optional<Vector<BlockFrameRange>> take_blocks_to_the_end_of_stream(Media::AudioProducer& producer, Core::EventLoop& loop, AK::Duration time_limit)
{
    Vector<BlockFrameRange> ranges;
    auto deadline = MonotonicTime::now_coarse() + time_limit;
    while (true) {
        auto output = producer.peek();
        if (output.status == Media::PipelineStatus::HaveData) {
            ranges.append({ output.block->first_frame_index(), output.block->end_frame_index() });
            producer.consume();
            continue;
        }
        if (output.status == Media::PipelineStatus::EndOfStream)
            return ranges;
        if (MonotonicTime::now_coarse() >= deadline)
            return {};
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
}

// The blocks a decoder produced before it lost its state are still good, so they stay available while decoding
// restarts, and decoding continues after them.
TEST_CASE(blocks_held_when_the_decoder_loses_its_state_stay_available)
{
    auto& loop = never_destroyed_event_loop();
    auto time_limit = AK::Duration::from_seconds(5);
    auto uninterrupted_ranges = take_blocks_to_the_end_of_stream(create_started_audio_producer(loop, "test-webm-xiph-lacing.mka"sv), loop, time_limit);
    if (!uninterrupted_ranges.has_value()) {
        FAIL("Timed out decoding to the end of the stream without losing the decoder state");
        return;
    }

    size_t const blocks_before_the_loss = 3;
    VERIFY(uninterrupted_ranges->size() > blocks_before_the_loss);
    auto state_loss = make_ref_counted<AudioDecoderStateLoss>(blocks_before_the_loss);
    StateLosingAudioDecoder::set_state_loss_for_new_decoders(state_loss);
    ScopeGuard stop_losing_state = [] { StateLosingAudioDecoder::set_state_loss_for_new_decoders(nullptr); };
    auto producer = create_started_audio_producer(loop, "test-webm-xiph-lacing.mka"sv);
    ScopeGuard release_held_decoding = [&] { state_loss->resume_decoding(); };

    if (!state_loss->wait_until_decoding_is_held_after_the_loss(time_limit)) {
        FAIL("Timed out waiting for decoding to restart after the decoder lost its state");
        return;
    }
    auto output = producer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::HaveData);
    state_loss->resume_decoding();

    auto ranges = take_blocks_to_the_end_of_stream(producer, loop, time_limit);
    if (!ranges.has_value()) {
        FAIL("Timed out decoding to the end of the stream after losing the decoder state");
        return;
    }
    VERIFY(ranges->size() > blocks_before_the_loss);
    for (size_t block_index = 0; block_index < blocks_before_the_loss; block_index++) {
        EXPECT_EQ(ranges->at(block_index).first_frame_index, uninterrupted_ranges->at(block_index).first_frame_index);
        EXPECT_EQ(ranges->at(block_index).end_frame_index, uninterrupted_ranges->at(block_index).end_frame_index);
    }
    // Decoding restarts on a coded frame boundary, so the first block after the held ones may overlap them.
    EXPECT(ranges->at(blocks_before_the_loss).first_frame_index <= ranges->at(blocks_before_the_loss - 1).end_frame_index);
    EXPECT(ranges->at(blocks_before_the_loss).end_frame_index > ranges->at(blocks_before_the_loss - 1).end_frame_index);
    EXPECT_EQ(ranges->last().end_frame_index, uninterrupted_ranges->last().end_frame_index);
}

TEST_CASE(a_seek_during_a_restart_lands_on_its_own_target)
{
    auto& loop = never_destroyed_event_loop();
    auto state_loss = make_ref_counted<AudioDecoderStateLoss>(3);
    StateLosingAudioDecoder::set_state_loss_for_new_decoders(state_loss);
    ScopeGuard stop_losing_state = [] { StateLosingAudioDecoder::set_state_loss_for_new_decoders(nullptr); };
    auto producer = create_started_audio_producer(loop, "test-webm-xiph-lacing.mka"sv);
    ScopeGuard release_held_decoding = [&] { state_loss->resume_decoding(); };

    auto time_limit = AK::Duration::from_seconds(5);
    if (!state_loss->wait_until_decoding_is_held_after_the_loss(time_limit)) {
        FAIL("Timed out waiting for decoding to restart after the decoder lost its state");
        return;
    }
    auto target = AK::Duration::from_seconds(5);
    producer->seek(target);
    state_loss->resume_decoding();

    auto deadline = MonotonicTime::now_coarse() + time_limit;
    auto output = producer->peek();
    while (output.status != Media::PipelineStatus::HaveData) {
        if (MonotonicTime::now_coarse() >= deadline) {
            FAIL("Timed out waiting for the seek to resolve");
            return;
        }
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        output = producer->peek();
    }
    EXPECT(output.block->contains_media_time(target));
}
