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

// Ends a track's samples after a number of them, with the given error.
class TruncatingDemuxer final : public Media::Demuxer {
public:
    static NonnullRefPtr<TruncatingDemuxer> create(NonnullRefPtr<Media::Demuxer> inner, size_t sample_count, Media::DecoderErrorCategory category)
    {
        return adopt_ref(*new TruncatingDemuxer(move(inner), sample_count, category));
    }

    virtual Media::DecoderErrorOr<void> create_context_for_track(Media::Track const& track) override { return m_inner->create_context_for_track(track); }
    virtual Media::DecoderErrorOr<Vector<Media::Track>> get_tracks_for_type(Media::TrackType type) override { return m_inner->get_tracks_for_type(type); }
    virtual Media::DecoderErrorOr<Optional<Media::Track>> get_preferred_track_for_type(Media::TrackType type) override { return m_inner->get_preferred_track_for_type(type); }
    virtual Media::DecoderErrorOr<Media::CodedFrame> get_next_sample_for_track(Media::Track const& track) override
    {
        if (m_samples_served >= m_sample_count)
            return Media::DecoderError::with_description(m_category, "The samples end here"sv);
        m_samples_served++;
        return m_inner->get_next_sample_for_track(track);
    }
    virtual AK::Duration select_fast_seek_target_for_track(Media::Track const& track, AK::Duration target, Media::SeekMode mode) override { return m_inner->select_fast_seek_target_for_track(track, target, mode); }
    virtual Media::DecoderErrorOr<Media::DemuxerSeekResult> seek_to_most_recent_keyframe(Media::Track const& track, AK::Duration timestamp, Media::DemuxerSeekOptions options) override { return m_inner->seek_to_most_recent_keyframe(track, timestamp, options); }
    virtual Media::DecoderErrorOr<AK::Duration> duration_of_track(Media::Track const& track) override { return m_inner->duration_of_track(track); }
    virtual Media::DecoderErrorOr<AK::Duration> total_duration() override { return m_inner->total_duration(); }
    virtual Media::DemuxerScanState const& scan_state() const LIFETIME_BOUND override { return m_inner->scan_state(); }
    virtual void set_scan_state_change_handler(ScanStateChangeHandler handler) override { m_inner->set_scan_state_change_handler(move(handler)); }
    virtual void set_blocking_reads_aborted_for_track(Media::Track const& track) override { m_inner->set_blocking_reads_aborted_for_track(track); }
    virtual void reset_blocking_reads_aborted_for_track(Media::Track const& track) override { m_inner->reset_blocking_reads_aborted_for_track(track); }
    virtual void set_read_blocked_change_handler_for_track(Media::Track const& track, Media::ReadBlockedChangeHandler handler) override { m_inner->set_read_blocked_change_handler_for_track(track, move(handler)); }

private:
    TruncatingDemuxer(NonnullRefPtr<Media::Demuxer> inner, size_t sample_count, Media::DecoderErrorCategory category)
        : m_inner(move(inner))
        , m_sample_count(sample_count)
        , m_category(category)
    {
    }

    NonnullRefPtr<Media::Demuxer> m_inner;
    size_t const m_sample_count;
    Media::DecoderErrorCategory const m_category;
    size_t m_samples_served { 0 };
};

static NonnullRefPtr<Media::DecodedAudioProducer> create_started_truncated_audio_producer(Core::EventLoop& loop, StringView path, size_t sample_count, Media::DecoderErrorCategory category, Optional<AK::Duration> seek_target = {})
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto inner_demuxer = MUST(Media::create_demuxer(stream));
    auto tracks = MUST(inner_demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());
    auto demuxer = TruncatingDemuxer::create(move(inner_demuxer), sample_count, category);
    auto producer = MUST(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0]));
    // Resampling holds back the end of the decoded audio until the converter is drained.
    MUST(producer->set_output_sample_specification(Audio::SampleSpecification(96'000, Audio::ChannelMap::stereo())));
    producer->set_error_handler([](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    if (seek_target.has_value())
        producer->seek(seek_target.value());
    producer->start();
    return producer;
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

// When the data runs out for good, the blocks the decoder still holds come out as they would at the end of the
// stream, but the stream does not end.
TEST_CASE(blocks_held_by_the_decoder_are_delivered_when_data_becomes_permanently_unavailable)
{
    auto& loop = never_destroyed_event_loop();
    auto time_limit = AK::Duration::from_seconds(5);
    size_t const sample_count = 20;

    auto ended_ranges = take_blocks_to_the_end_of_stream(create_started_truncated_audio_producer(loop, "test-webm-xiph-lacing.mka"sv, sample_count, Media::DecoderErrorCategory::EndOfStream), loop, time_limit);
    if (!ended_ranges.has_value()) {
        FAIL("Timed out decoding to the end of the truncated stream");
        return;
    }

    auto producer = create_started_truncated_audio_producer(loop, "test-webm-xiph-lacing.mka"sv, sample_count, Media::DecoderErrorCategory::PermanentlyUnavailable);
    Vector<BlockFrameRange> ranges;
    auto quiet_period = AK::Duration::from_milliseconds(300);
    auto deadline = MonotonicTime::now_coarse() + time_limit;
    auto quiet_until = MonotonicTime::now_coarse() + quiet_period;
    while (MonotonicTime::now_coarse() < quiet_until && MonotonicTime::now_coarse() < deadline) {
        auto output = producer->peek();
        EXPECT_NE(output.status, Media::PipelineStatus::EndOfStream);
        if (output.status == Media::PipelineStatus::HaveData) {
            ranges.append({ output.block->first_frame_index(), output.block->end_frame_index() });
            producer->consume();
            quiet_until = MonotonicTime::now_coarse() + quiet_period;
            continue;
        }
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    EXPECT_EQ(ranges.size(), ended_ranges->size());
    if (!ranges.is_empty() && !ended_ranges->is_empty())
        EXPECT_EQ(ranges.last().end_frame_index, ended_ranges->last().end_frame_index);
}

// The end of the audio before the data runs out only comes out of the converter when it is drained, so a seek to it
// resolves once the decoder drains.
TEST_CASE(a_seek_resolves_on_held_blocks_when_data_becomes_permanently_unavailable)
{
    auto& loop = never_destroyed_event_loop();
    auto time_limit = AK::Duration::from_seconds(5);
    // Few enough samples that the target lies within the seek pre-roll, so that both producers decode from the first
    // sample.
    size_t const sample_count = 4;

    auto ended_ranges = take_blocks_to_the_end_of_stream(create_started_truncated_audio_producer(loop, "test-webm-xiph-lacing.mka"sv, sample_count, Media::DecoderErrorCategory::EndOfStream), loop, time_limit);
    if (!ended_ranges.has_value() || ended_ranges->is_empty()) {
        FAIL("Timed out decoding to the end of the truncated stream");
        return;
    }
    auto target = AK::Duration::from_time_units(ended_ranges->last().end_frame_index - 1, 1, 96'000);

    auto producer = create_started_truncated_audio_producer(loop, "test-webm-xiph-lacing.mka"sv, sample_count, Media::DecoderErrorCategory::PermanentlyUnavailable, target);
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
