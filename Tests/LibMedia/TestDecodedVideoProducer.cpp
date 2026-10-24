/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/Mutex.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/System.h>
#include <LibMedia/DecoderRegistrations.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMediaCodecs/VideoFramePool.h>
#include <LibThreading/Thread.h>
#include <Tests/LibMedia/TestMediaCommon.h>
#include <Tests/LibMedia/WithholdingVideoDecoder.h>

TEST_SETUP
{
    Media::install_video_decoders({ WithholdingVideoDecoder::registrations(), Media::hardware_video_decoders(), Media::ffmpeg_video_decoders() });
    Media::install_audio_decoders({ Media::audio_decoders() });
}

namespace {

// Serves a number of a demuxer's samples and then blocks every read until it is aborted, like a stream that has run
// out of data to read.
class BlockingDemuxer final : public Media::Demuxer {
public:
    static NonnullRefPtr<BlockingDemuxer> create(NonnullRefPtr<Media::Demuxer> inner, size_t samples_before_blocking)
    {
        return adopt_ref(*new BlockingDemuxer(move(inner), samples_before_blocking));
    }

    virtual Media::DecoderErrorOr<void> create_context_for_track(Media::Track const& track) override { return m_inner->create_context_for_track(track); }
    virtual Media::DecoderErrorOr<Vector<Media::Track>> get_tracks_for_type(Media::TrackType type) override { return m_inner->get_tracks_for_type(type); }
    virtual Media::DecoderErrorOr<Optional<Media::Track>> get_preferred_track_for_type(Media::TrackType type) override { return m_inner->get_preferred_track_for_type(type); }

    virtual Media::DecoderErrorOr<Media::CodedFrame> get_next_sample_for_track(Media::Track const& track) override
    {
        {
            MutexLocker locker { m_mutex };
            if (m_samples_served >= m_samples_before_blocking) {
                m_read_is_blocked = true;
                m_state_changed.broadcast();
                while (m_blocks_reads && !m_reads_are_aborted)
                    m_state_changed.wait();
                m_read_is_blocked = false;
                if (m_reads_are_aborted)
                    return Media::DecoderError::with_description(Media::DecoderErrorCategory::Aborted, "Blocking read was aborted"sv);
            }
            m_samples_served++;
        }
        if (m_seek_is_expected.load())
            m_reads_before_the_expected_seek++;
        return m_inner->get_next_sample_for_track(track);
    }

    virtual AK::Duration select_fast_seek_target_for_track(Media::Track const& track, AK::Duration target, Media::SeekMode mode) override { return m_inner->select_fast_seek_target_for_track(track, target, mode); }
    virtual Media::DecoderErrorOr<Media::DemuxerSeekResult> seek_to_most_recent_keyframe(Media::Track const& track, AK::Duration timestamp, Media::DemuxerSeekOptions options) override
    {
        m_seek_is_expected.store(false);
        m_seek_count++;
        if (has_flag(options, Media::DemuxerSeekOptions::Force))
            m_forced_seek_count++;
        return m_inner->seek_to_most_recent_keyframe(track, timestamp, options);
    }
    virtual Media::DecoderErrorOr<AK::Duration> duration_of_track(Media::Track const& track) override { return m_inner->duration_of_track(track); }
    virtual Media::DecoderErrorOr<AK::Duration> total_duration() override { return m_inner->total_duration(); }
    virtual Media::DemuxerScanState const& scan_state() const LIFETIME_BOUND override { return m_inner->scan_state(); }
    virtual void set_scan_state_change_handler(ScanStateChangeHandler handler) override { m_inner->set_scan_state_change_handler(move(handler)); }

    virtual void set_blocking_reads_aborted_for_track(Media::Track const& track) override
    {
        {
            MutexLocker locker { m_mutex };
            m_reads_are_aborted = true;
            m_state_changed.broadcast();
        }
        m_inner->set_blocking_reads_aborted_for_track(track);
    }
    virtual void reset_blocking_reads_aborted_for_track(Media::Track const& track) override
    {
        {
            MutexLocker locker { m_mutex };
            m_reads_are_aborted = false;
        }
        m_inner->reset_blocking_reads_aborted_for_track(track);
    }
    virtual void set_read_blocked_change_handler_for_track(Media::Track const& track, Media::ReadBlockedChangeHandler handler) override { m_inner->set_read_blocked_change_handler_for_track(track, move(handler)); }

    bool wait_until_a_read_blocks(AK::Duration time_limit)
    {
        auto deadline = MonotonicTime::now() + time_limit;
        MutexLocker locker { m_mutex };
        while (!m_read_is_blocked) {
            auto now = MonotonicTime::now();
            if (now >= deadline)
                return false;
            m_state_changed.wait_for(deadline - now);
        }
        return true;
    }

    void stop_blocking_reads()
    {
        MutexLocker locker { m_mutex };
        m_blocks_reads = false;
        m_state_changed.broadcast();
    }

    size_t samples_served()
    {
        MutexLocker locker { m_mutex };
        return m_samples_served;
    }
    size_t seek_count() const { return m_seek_count; }
    size_t forced_seek_count() const { return m_forced_seek_count; }

    void expect_seek() { m_seek_is_expected.store(true); }
    size_t reads_before_the_expected_seek() const { return m_reads_before_the_expected_seek; }

private:
    BlockingDemuxer(NonnullRefPtr<Media::Demuxer> inner, size_t samples_before_blocking)
        : m_inner(move(inner))
        , m_samples_before_blocking(samples_before_blocking)
    {
    }

    NonnullRefPtr<Media::Demuxer> m_inner;
    size_t const m_samples_before_blocking { 0 };

    Mutex m_mutex;
    ConditionVariable m_state_changed { m_mutex };
    size_t m_samples_served { 0 };
    bool m_blocks_reads { true };
    bool m_reads_are_aborted { false };
    bool m_read_is_blocked { false };
    Atomic<size_t> m_seek_count { 0 };
    Atomic<size_t> m_forced_seek_count { 0 };
    Atomic<bool> m_seek_is_expected { false };
    Atomic<size_t> m_reads_before_the_expected_seek { 0 };
};

// Presents one track whose coded frames come from one demuxer and then another, so that the codec of a track's
// frames changes partway through as it does when a new initialization segment selects a different one.
class SwitchingDemuxer final : public Media::Demuxer {
public:
    static NonnullRefPtr<SwitchingDemuxer> create(NonnullRefPtr<Media::Demuxer> first, Media::Track first_track, NonnullRefPtr<Media::Demuxer> second, Media::Track second_track)
    {
        return adopt_ref(*new SwitchingDemuxer(move(first), first_track, move(second), second_track));
    }

    virtual Media::DecoderErrorOr<void> create_context_for_track(Media::Track const&) override
    {
        TRY(m_first->create_context_for_track(m_first_track));
        return m_second->create_context_for_track(m_second_track);
    }

    virtual Media::DecoderErrorOr<Vector<Media::Track>> get_tracks_for_type(Media::TrackType type) override
    {
        if (type != Media::TrackType::Video)
            return Vector<Media::Track> {};
        return Vector<Media::Track> { m_first_track };
    }

    virtual Media::DecoderErrorOr<Optional<Media::Track>> get_preferred_track_for_type(Media::TrackType type) override
    {
        if (type != Media::TrackType::Video)
            return Optional<Media::Track> {};
        return Optional<Media::Track> { m_first_track };
    }

    virtual Media::DecoderErrorOr<Media::CodedFrame> get_next_sample_for_track(Media::Track const&) override
    {
        if (!m_switched) {
            auto sample_result = m_first->get_next_sample_for_track(m_first_track);
            if (!sample_result.is_error()) {
                auto sample = sample_result.release_value();
                m_switch_timestamp = sample.presentation_timestamp() + sample.duration();
                m_first_frame_count++;
                return sample;
            }
            if (sample_result.error().category() != Media::DecoderErrorCategory::EndOfStream)
                return sample_result.release_error();
            m_switched = true;
        }

        auto sample = TRY(m_second->get_next_sample_for_track(m_second_track));
        m_second_frame_count++;
        // The second demuxer's timestamps restart at zero, so they are shifted to continue after the first's.
        return Media::CodedFrame {
            sample.codec_id(),
            m_switch_timestamp + sample.presentation_timestamp(),
            m_switch_timestamp + sample.decode_timestamp(),
            sample.duration(),
            sample.flags(),
            MUST(FixedArray<u8>::create(sample.data())),
            sample.new_codec_configuration().has_value() ? Optional<FixedArray<u8>> { MUST(FixedArray<u8>::create(*sample.new_codec_configuration())) } : Optional<FixedArray<u8>> {},
        };
    }

    virtual AK::Duration select_fast_seek_target_for_track(Media::Track const&, AK::Duration target, Media::SeekMode) override { return target; }
    virtual Media::DecoderErrorOr<Media::DemuxerSeekResult> seek_to_most_recent_keyframe(Media::Track const&, AK::Duration, Media::DemuxerSeekOptions) override
    {
        return Media::DemuxerSeekResult::KeptCurrentPosition;
    }

    virtual Media::DecoderErrorOr<AK::Duration> duration_of_track(Media::Track const&) override { return AK::Duration::zero(); }
    virtual Media::DecoderErrorOr<AK::Duration> total_duration() override { return AK::Duration::zero(); }

    virtual Media::DemuxerScanState const& scan_state() const LIFETIME_BOUND override { return m_scan_state; }
    virtual void set_scan_state_change_handler(ScanStateChangeHandler) override { }

    virtual void set_blocking_reads_aborted_for_track(Media::Track const&) override { }
    virtual void reset_blocking_reads_aborted_for_track(Media::Track const&) override { }
    virtual void set_read_blocked_change_handler_for_track(Media::Track const&, Media::ReadBlockedChangeHandler) override { }

    size_t first_frame_count() const { return m_first_frame_count; }
    size_t second_frame_count() const { return m_second_frame_count; }

private:
    SwitchingDemuxer(NonnullRefPtr<Media::Demuxer> first, Media::Track first_track, NonnullRefPtr<Media::Demuxer> second, Media::Track second_track)
        : m_first(move(first))
        , m_first_track(first_track)
        , m_second(move(second))
        , m_second_track(second_track)
    {
    }

    NonnullRefPtr<Media::Demuxer> m_first;
    Media::Track m_first_track;
    NonnullRefPtr<Media::Demuxer> m_second;
    Media::Track m_second_track;

    bool m_switched { false };
    AK::Duration m_switch_timestamp;
    size_t m_first_frame_count { 0 };
    size_t m_second_frame_count { 0 };
    Media::DemuxerScanState m_scan_state;
};

// Serves a demuxer's samples again after its first end of stream, like a stream that grew after its end was read.
class ResumingDemuxer final : public Media::Demuxer {
public:
    static NonnullRefPtr<ResumingDemuxer> create(NonnullRefPtr<Media::Demuxer> inner)
    {
        return adopt_ref(*new ResumingDemuxer(move(inner)));
    }

    virtual Media::DecoderErrorOr<void> create_context_for_track(Media::Track const& track) override { return m_inner->create_context_for_track(track); }
    virtual Media::DecoderErrorOr<Vector<Media::Track>> get_tracks_for_type(Media::TrackType type) override { return m_inner->get_tracks_for_type(type); }
    virtual Media::DecoderErrorOr<Optional<Media::Track>> get_preferred_track_for_type(Media::TrackType type) override { return m_inner->get_preferred_track_for_type(type); }

    virtual Media::DecoderErrorOr<Media::CodedFrame> get_next_sample_for_track(Media::Track const& track) override
    {
        auto sample = m_inner->get_next_sample_for_track(track);
        if (!sample.is_error()) {
            if (m_resumed)
                m_resumed_sample_count++;
            return sample;
        }
        if (m_resumed || sample.error().category() != Media::DecoderErrorCategory::EndOfStream)
            return sample.release_error();
        m_resumed = true;
        TRY(m_inner->seek_to_most_recent_keyframe(track, AK::Duration::zero(), Media::DemuxerSeekOptions::Force));
        return sample.release_error();
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

    size_t resumed_sample_count() const { return m_resumed_sample_count; }

private:
    explicit ResumingDemuxer(NonnullRefPtr<Media::Demuxer> inner)
        : m_inner(move(inner))
    {
    }

    NonnullRefPtr<Media::Demuxer> m_inner;
    bool m_resumed { false };
    Atomic<size_t> m_resumed_sample_count { 0 };
};

struct DemuxerAndTrack {
    NonnullRefPtr<Media::Demuxer> demuxer;
    Media::Track track;
};

DemuxerAndTrack demuxer_and_video_track_for(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST(Media::create_demuxer(stream));
    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());
    return { move(demuxer), tracks[0] };
}

NonnullRefPtr<Media::DecodedVideoProducer> create_started_producer(Core::EventLoop& loop, StringView path)
{
    auto [demuxer, track] = demuxer_and_video_track_for(path);
    auto producer = MUST(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->start();
    return producer;
}

Vector<AK::Duration> presentation_timestamps_of_first_samples(StringView path, size_t sample_count)
{
    auto [demuxer, track] = demuxer_and_video_track_for(path);
    MUST(demuxer->create_context_for_track(track));
    Vector<AK::Duration> timestamps;
    for (size_t sample_index = 0; sample_index < sample_count; sample_index++) {
        auto sample = demuxer->get_next_sample_for_track(track);
        if (sample.is_error())
            break;
        timestamps.append(sample.value().presentation_timestamp());
    }
    return timestamps;
}

// Releases withheld outputs from another thread a few milliseconds after they arrive, the way a media engine finishes
// frames on threads of its own.
class AsynchronousOutputReleaser {
public:
    explicit AsynchronousOutputReleaser(NonnullRefPtr<WithheldOutputs> withheld_outputs)
        : m_withheld_outputs(move(withheld_outputs))
    {
        m_thread = MUST(Threading::Thread::try_create("Release"sv, [this]() -> intptr_t {
            while (!m_stop.load()) {
                MUST(Core::System::sleep_ms(get_random_uniform(4)));
                m_withheld_outputs->release_next();
            }
            return 0;
        }));
        m_thread->start();
    }

    ~AsynchronousOutputReleaser()
    {
        m_stop.store(true);
        (void)m_thread->join();
    }

private:
    NonnullRefPtr<WithheldOutputs> m_withheld_outputs;
    Atomic<bool> m_stop { false };
    RefPtr<Threading::Thread> m_thread;
};

RefPtr<Media::VideoFrame> take_frame_within_time_limit(Media::VideoProducer& producer, Core::EventLoop& loop, AK::Duration time_limit)
{
    auto start_time = MonotonicTime::now_coarse();
    while (MonotonicTime::now_coarse() - start_time < time_limit) {
        auto output = producer.peek();
        if (output.status == Media::PipelineStatus::HaveData) {
            producer.consume();
            return output.frame;
        }
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    return nullptr;
}

}

TEST_CASE(decoding_resumes_when_a_held_frame_releases_its_storage)
{
    auto& loop = never_destroyed_event_loop();

    auto [demuxer, track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->start();

    auto time_limit = AK::Duration::from_seconds(3);

    // Holding a frame from every slot the pool can allocate leaves the decoder with nowhere to put its next one.
    Vector<NonnullRefPtr<Media::VideoFrame>> held_frames;
    for (u32 index = 0; index < Media::VideoFramePool::MAX_SLOT_COUNT; index++) {
        auto frame = take_frame_within_time_limit(*producer, loop, time_limit);
        if (frame == nullptr) {
            FAIL(ByteString::formatted("Timed out waiting for frame {}", index));
            return;
        }
        held_frames.append(frame.release_nonnull());
    }

    // No further frame can exist while every slot is held, however long the decoder is given to produce one.
    auto backpressure_deadline = MonotonicTime::now_coarse() + AK::Duration::from_milliseconds(100);
    while (MonotonicTime::now_coarse() < backpressure_deadline) {
        EXPECT_NE(producer->peek().status, Media::PipelineStatus::HaveData);
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    (void)held_frames.take_first();

    if (take_frame_within_time_limit(*producer, loop, time_limit) == nullptr)
        FAIL("Timed out waiting for decoding to resume after a frame was released");
}

// Once the last frame has been consumed, nothing downstream is guaranteed to still hold it, so a seek past it must
// re-emit that same frame rather than resolve with none, however many times it is asked.
TEST_CASE(a_seek_past_the_consumed_last_frame_re_emits_it)
{
    auto& loop = never_destroyed_event_loop();
    auto producer = create_started_producer(loop, "./vp9_in_webm.webm"sv);
    auto time_limit = AK::Duration::from_seconds(10);

    auto start_time = MonotonicTime::now_coarse();
    RefPtr<Media::VideoFrame> last_frame;
    while (true) {
        auto output = producer->peek();
        if (output.status == Media::PipelineStatus::HaveData) {
            last_frame = output.frame;
            producer->consume();
        } else if (output.status == Media::PipelineStatus::EndOfStream) {
            break;
        }
        if (MonotonicTime::now_coarse() - start_time >= time_limit) {
            FAIL("Timed out decoding to the end of the stream");
            return;
        }
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    EXPECT(last_frame != nullptr);

    for (int attempt = 0; attempt < 2; attempt++) {
        producer->seek(last_frame->timestamp() + AK::Duration::from_seconds(1 + attempt));
        auto output = producer->peek();
        EXPECT_EQ(output.status, Media::PipelineStatus::HaveData);
        EXPECT(output.frame.ptr() == last_frame.ptr());
        producer->consume();
        EXPECT_EQ(producer->peek().status, Media::PipelineStatus::EndOfStream);
    }
}

// A decoder keeps frames meant for output across a seek that leaves the demuxer where it was, and a seek past the
// end has none of its own to offer instead, so those must not pass for the last frame.
TEST_CASE(a_seek_past_the_end_during_playback_lands_on_the_last_frame)
{
    auto& loop = never_destroyed_event_loop();

    AK::Duration last_sample_timestamp;
    {
        auto [demuxer, track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
        TRY_OR_FAIL(demuxer->create_context_for_track(track));
        while (true) {
            auto sample = demuxer->get_next_sample_for_track(track);
            if (sample.is_error())
                break;
            last_sample_timestamp = sample.value().presentation_timestamp();
        }
    }

    auto producer = create_started_producer(loop, "./vp9_in_webm.webm"sv);
    auto time_limit = AK::Duration::from_seconds(5);
    // Decoding has to be underway for the decoder to hold frames meant for output when the seek arrives.
    for (size_t frame_count = 0; frame_count < 3; frame_count++) {
        if (take_frame_within_time_limit(*producer, loop, time_limit) == nullptr) {
            FAIL(ByteString::formatted("Timed out waiting for frame {}", frame_count));
            return;
        }
    }

    producer->seek(last_sample_timestamp + AK::Duration::from_seconds(10));
    auto frame = take_frame_within_time_limit(*producer, loop, time_limit);
    if (frame == nullptr)
        FAIL("Timed out waiting for the seek past the end to resolve");
    else
        EXPECT_EQ(frame->timestamp(), last_sample_timestamp);
}

TEST_CASE(a_codec_change_drains_the_previous_decoder)
{
    auto& loop = never_destroyed_event_loop();

    auto [av1_demuxer, av1_track] = demuxer_and_video_track_for("./av1_in_webm.webm"sv);
    auto [vp9_demuxer, vp9_track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);

    auto switching_demuxer = SwitchingDemuxer::create(av1_demuxer, av1_track, vp9_demuxer, vp9_track);
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, switching_demuxer, av1_track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->start();

    auto time_limit = AK::Duration::from_seconds(10);
    auto start_time = MonotonicTime::now_coarse();
    size_t decoded_frame_count = 0;

    while (true) {
        auto output = producer->peek();
        if (output.status == Media::PipelineStatus::HaveData) {
            decoded_frame_count++;
            producer->consume();
        } else if (output.status == Media::PipelineStatus::EndOfStream) {
            break;
        }

        if (MonotonicTime::now_coarse() - start_time >= time_limit) {
            FAIL("Decoding timed out.");
            return;
        }

        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    // Both codecs' frames are decoded, so none of the frames the first decoder still held when the codec
    // changed were dropped along with it.
    EXPECT(switching_demuxer->first_frame_count() > 0);
    EXPECT(switching_demuxer->second_frame_count() > 0);
    EXPECT_EQ(decoded_frame_count, switching_demuxer->first_frame_count() + switching_demuxer->second_frame_count());
}

// A seek can resolve on a frame that the previous decoder outputs while it drains for a codec change. The frame that
// changed the codec has to wait for that drain to finish before decoding continues.
TEST_CASE(a_seek_that_resolves_while_the_decoder_drains_for_a_codec_change_keeps_decoding)
{
    auto& loop = never_destroyed_event_loop();

    auto [av1_demuxer, av1_track] = demuxer_and_video_track_for("./av1_in_webm.webm"sv);
    auto [vp9_demuxer, vp9_track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
    auto av1_duration = TRY_OR_FAIL(av1_demuxer->duration_of_track(av1_track));

    auto switching_demuxer = SwitchingDemuxer::create(av1_demuxer, av1_track, vp9_demuxer, vp9_track);
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, switching_demuxer, av1_track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->seek(av1_duration - AK::Duration::from_milliseconds(1));
    producer->start();

    auto time_limit = AK::Duration::from_seconds(10);
    for (size_t frame_count = 0; frame_count < 3; frame_count++) {
        if (take_frame_within_time_limit(*producer, loop, time_limit) == nullptr) {
            FAIL(ByteString::formatted("Timed out waiting for frame {}", frame_count));
            return;
        }
    }
    EXPECT(switching_demuxer->second_frame_count() > 0);
}

// A stream can move to a format the decoder in use has no support for, and only the first frame after a seek carries
// a codec configuration, so replacing that decoder means asking the demuxer for one again.
TEST_CASE(a_decoder_that_cannot_decode_a_later_format_is_replaced)
{
    auto& loop = never_destroyed_event_loop();
    auto producer = create_started_producer(loop, "./vp9_profile_change.webm"sv);

    auto time_limit = AK::Duration::from_seconds(5);
    size_t frame_count = 0;
    while (frame_count < 10) {
        auto frame = take_frame_within_time_limit(*producer, loop, time_limit);
        if (frame == nullptr) {
            FAIL(ByteString::formatted("Timed out waiting for frame {}", frame_count));
            return;
        }
        frame_count++;
    }
}

// A decoder that finishes its output on threads of its own can do so while the decode thread waits for the demuxer,
// as it does at the end of the data appended so far. That output has to reach the queue without another coded frame.
TEST_CASE(output_that_arrives_while_the_demuxer_blocks_is_delivered)
{
    auto& loop = never_destroyed_event_loop();
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };

    auto sample_timestamps = presentation_timestamps_of_first_samples("./vp9_in_webm.webm"sv, 3);
    auto [inner_demuxer, track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
    auto demuxer = BlockingDemuxer::create(inner_demuxer, sample_timestamps.size());
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->start();

    auto time_limit = AK::Duration::from_seconds(5);
    if (!demuxer->wait_until_a_read_blocks(time_limit)) {
        FAIL("Timed out waiting for the decode thread to block on the demuxer");
        return;
    }
    EXPECT_EQ(withheld_outputs->withheld_count(), sample_timestamps.size());
    EXPECT_NE(producer->peek().status, Media::PipelineStatus::HaveData);

    EXPECT(withheld_outputs->release_next());
    auto frame = take_frame_within_time_limit(*producer, loop, time_limit);
    if (frame == nullptr)
        FAIL("The output that arrived while the demuxer blocked was not delivered");
    else
        EXPECT_EQ(frame->timestamp(), sample_timestamps[0]);

    demuxer->stop_blocking_reads();
}

// The frame a seek resolves on can arrive the same way, and must resolve the seek without decoding from its keyframe
// again.
TEST_CASE(a_seek_resolves_on_output_that_arrives_while_the_demuxer_blocks)
{
    auto& loop = never_destroyed_event_loop();
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };

    auto sample_timestamps = presentation_timestamps_of_first_samples("./vp9_in_webm.webm"sv, 3);
    auto target_sample_timestamp = sample_timestamps.last();
    auto [inner_demuxer, track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
    auto demuxer = BlockingDemuxer::create(inner_demuxer, sample_timestamps.size());
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->seek(target_sample_timestamp + AK::Duration::from_milliseconds(1));
    producer->start();

    auto time_limit = AK::Duration::from_seconds(5);
    if (!demuxer->wait_until_a_read_blocks(time_limit)) {
        FAIL("Timed out waiting for the decode thread to block on the demuxer");
        return;
    }
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Pending);

    // The samples before the block cover the target, so releasing their outputs is all the seek needs.
    auto deadline = MonotonicTime::now_coarse() + time_limit;
    RefPtr<Media::VideoFrame> frame;
    while (frame == nullptr && MonotonicTime::now_coarse() < deadline) {
        auto output = producer->peek();
        if (output.status == Media::PipelineStatus::HaveData) {
            frame = output.frame;
            break;
        }
        if (!withheld_outputs->release_next())
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    if (frame == nullptr)
        FAIL("The seek did not resolve on the output that arrived while the demuxer blocked");
    else
        EXPECT_EQ(frame->timestamp(), target_sample_timestamp);
    EXPECT_EQ(demuxer->seek_count(), 1u);

    demuxer->stop_blocking_reads();
}

// Output finishing while a seek is requested wakes the decode thread between taking output and reading the next
// sample. It must not read, and so submit, another sample of the old position before it seeks.
TEST_CASE(no_further_sample_is_read_once_a_seek_is_requested)
{
    auto& loop = never_destroyed_event_loop();
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };

    auto [inner_demuxer, track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
    auto demuxer = BlockingDemuxer::create(inner_demuxer, NumericLimits<size_t>::max());
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->start();

    // Once as many outputs are withheld as the decoder decodes at once, the decode thread waits in it for one.
    auto time_limit = AK::Duration::from_seconds(5);
    if (!withheld_outputs->wait_until_the_caller_waits_for_output(time_limit)) {
        FAIL("Timed out waiting for the decode thread to wait for output");
        return;
    }
    EXPECT_EQ(withheld_outputs->withheld_count(), WithholdingVideoDecoder::MAXIMUM_WITHHELD_OUTPUTS);

    demuxer->expect_seek();
    producer->seek(AK::Duration::from_milliseconds(500));
    EXPECT(withheld_outputs->release_next());

    auto deadline = MonotonicTime::now_coarse() + time_limit;
    while (demuxer->seek_count() == 0 && MonotonicTime::now_coarse() < deadline)
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(demuxer->seek_count(), 1u);
    EXPECT_EQ(demuxer->reads_before_the_expected_seek(), 0u);
}

// Output can finish after a seek is requested but before the decode thread handles it. If that output covers the target,
// the seek resolves on it without moving the demuxer.
TEST_CASE(a_seek_resolves_on_output_queued_before_the_decode_thread_handles_it)
{
    auto& loop = never_destroyed_event_loop();
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };

    auto sample_timestamps = presentation_timestamps_of_first_samples("./vp9_in_webm.webm"sv, 1);
    VERIFY(sample_timestamps.size() == 1);
    auto [inner_demuxer, track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
    auto demuxer = BlockingDemuxer::create(inner_demuxer, NumericLimits<size_t>::max());
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->start();

    auto time_limit = AK::Duration::from_seconds(5);
    if (!withheld_outputs->wait_until_the_caller_waits_for_output(time_limit)) {
        FAIL("Timed out waiting for the decode thread to wait for output");
        return;
    }

    // Nothing is queued yet, so the seek can't resolve when it is requested.
    producer->seek(sample_timestamps[0]);
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Pending);

    // The decode thread is waiting in the decoder, so it queues this output before it handles the seek.
    EXPECT(withheld_outputs->release_next());

    auto deadline = MonotonicTime::now_coarse() + time_limit;
    RefPtr<Media::VideoFrame> frame;
    while (frame == nullptr && demuxer->seek_count() == 0 && MonotonicTime::now_coarse() < deadline) {
        auto output = producer->peek();
        if (output.status == Media::PipelineStatus::HaveData)
            frame = output.frame;
        else
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    EXPECT_EQ(demuxer->seek_count(), 0u);
    if (frame == nullptr) {
        FAIL("The seek did not resolve on the queued output");
        return;
    }
    EXPECT_EQ(frame->timestamp(), sample_timestamps[0]);
}

// A decoder finishing output on its own threads wakes the decode thread at arbitrary points of its loops. Seeking back
// and forth through such a decoder must land on the frame covering each target every time, and never report the end
// of the stream or stop answering.
TEST_CASE(seeks_land_on_the_right_frame_while_output_arrives_asynchronously)
{
    auto& loop = never_destroyed_event_loop();
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };

    auto sample_timestamps = presentation_timestamps_of_first_samples("./vp9_in_webm.webm"sv, NumericLimits<size_t>::max());
    VERIFY(sample_timestamps.size() > 10);
    auto expected_frame_timestamp_for = [&](AK::Duration target) {
        auto expected = sample_timestamps.first();
        for (auto timestamp : sample_timestamps) {
            if (timestamp <= target)
                expected = timestamp;
        }
        return expected;
    };

    auto producer = create_started_producer(loop, "./vp9_in_webm.webm"sv);
    AsynchronousOutputReleaser releaser { withheld_outputs };

    auto time_limit = AK::Duration::from_seconds(5);
    auto first_timestamp = sample_timestamps.first();
    auto last_timestamp = sample_timestamps.last();
    for (size_t seek_index = 0; seek_index < 300; seek_index++) {
        auto target = first_timestamp + AK::Duration::from_milliseconds(get_random_uniform((last_timestamp - first_timestamp).to_milliseconds()));
        producer->seek(target);

        // Like a sink, consume the frames that end before the target and show the first that does not.
        auto deadline = MonotonicTime::now_coarse() + time_limit;
        RefPtr<Media::VideoFrame> frame;
        while (MonotonicTime::now_coarse() < deadline) {
            auto output = producer->peek();
            if (output.status == Media::PipelineStatus::HaveData) {
                if (output.frame->end() > target) {
                    frame = output.frame;
                    break;
                }
                producer->consume();
                continue;
            }
            if (output.status == Media::PipelineStatus::Error) {
                FAIL(ByteString::formatted("Seek {} to {}ms ended in an error", seek_index, target.to_milliseconds()));
                return;
            }
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }
        if (frame == nullptr) {
            FAIL(ByteString::formatted("Seek {} to {}ms did not resolve", seek_index, target.to_milliseconds()));
            return;
        }
        if (frame->timestamp() != expected_frame_timestamp_for(target)) {
            FAIL(ByteString::formatted("Seek {} to {}ms resolved on the frame at {}ms instead of {}ms", seek_index, target.to_milliseconds(), frame->timestamp().to_milliseconds(), expected_frame_timestamp_for(target).to_milliseconds()));
            return;
        }
    }
}

// Seeking back to an earlier time within the frame of the last sample the demuxer read lets the demuxer keep its
// position, so the decoder will not output that frame again. The seek must still land on it.
TEST_CASE(a_seek_back_within_the_last_decoded_frame_lands_on_it)
{
    auto& loop = never_destroyed_event_loop();
    // A decoder that delays its output would hold back the last frame read before the demuxer blocks.
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };

    auto sample_timestamps = presentation_timestamps_of_first_samples("./vp9_in_webm.webm"sv, 4);
    VERIFY(sample_timestamps.size() == 4);
    auto last_read_sample_timestamp = sample_timestamps[2];
    auto frame_interval_in_milliseconds = (sample_timestamps[3] - last_read_sample_timestamp).to_milliseconds();

    auto [inner_demuxer, track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
    auto demuxer = BlockingDemuxer::create(inner_demuxer, 3);
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->seek(last_read_sample_timestamp + AK::Duration::from_milliseconds(frame_interval_in_milliseconds * 3 / 4));
    producer->start();
    AsynchronousOutputReleaser releaser { withheld_outputs };

    auto time_limit = AK::Duration::from_seconds(5);
    auto peek_frame = [&] -> RefPtr<Media::VideoFrame> {
        auto deadline = MonotonicTime::now_coarse() + time_limit;
        while (MonotonicTime::now_coarse() < deadline) {
            auto output = producer->peek();
            if (output.status == Media::PipelineStatus::HaveData)
                return output.frame;
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }
        return nullptr;
    };

    auto frame = peek_frame();
    if (frame == nullptr) {
        FAIL("The first seek did not resolve");
        return;
    }
    EXPECT_EQ(frame->timestamp(), last_read_sample_timestamp);
    if (!demuxer->wait_until_a_read_blocks(time_limit)) {
        FAIL("Timed out waiting for the decode thread to block on the demuxer");
        return;
    }

    producer->seek(last_read_sample_timestamp + AK::Duration::from_milliseconds(frame_interval_in_milliseconds / 4));
    demuxer->stop_blocking_reads();
    frame = peek_frame();
    if (frame == nullptr) {
        FAIL("The second seek did not resolve");
        return;
    }
    EXPECT_EQ(frame->timestamp(), last_read_sample_timestamp);
}

// A stream can grow after the demuxer reported its end, by which point the decoder was drained on that report.
// Nothing can be decoded from the samples that follow, so they are ignored rather than ending playback in an error.
// The decoder's references were made from the data that a seek invalidates, so it cannot continue from the demuxer's
// position even when the target is ahead of it.
TEST_CASE(a_seek_that_invalidates_held_data_moves_the_demuxer)
{
    auto& loop = never_destroyed_event_loop();
    auto sample_timestamps = presentation_timestamps_of_first_samples("./vp9_in_webm.webm"sv, 4);
    VERIFY(sample_timestamps.size() == 4);

    auto [inner_demuxer, track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
    auto demuxer = BlockingDemuxer::create(inner_demuxer, 3);
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->start();

    auto time_limit = AK::Duration::from_seconds(5);
    if (!demuxer->wait_until_a_read_blocks(time_limit)) {
        FAIL("Timed out waiting for the demuxer to block");
        return;
    }

    auto forced_seek_count_before_invalidation = demuxer->forced_seek_count();
    producer->seek(sample_timestamps[3], Media::InvalidateHeldData::Yes);
    demuxer->stop_blocking_reads();
    auto frame = take_frame_within_time_limit(*producer, loop, time_limit);
    if (frame == nullptr) {
        FAIL("Timed out waiting for the seek to resolve");
        return;
    }
    EXPECT_EQ(frame->timestamp(), sample_timestamps[3]);
    EXPECT_EQ(demuxer->forced_seek_count(), forced_seek_count_before_invalidation + 1);
}

// A suspended producer has released its decoder, so the decoder it creates to resume has to start from a keyframe, even
// when the demuxer could otherwise keep its position for the target.
TEST_CASE(resuming_from_suspension_partway_into_a_group_of_pictures_moves_the_demuxer)
{
    auto& loop = never_destroyed_event_loop();

    struct Sample {
        AK::Duration timestamp;
        bool is_keyframe;
    };
    Vector<Sample> samples;
    {
        auto [demuxer, track] = demuxer_and_video_track_for("./big_buck_bunny_5s.webm"sv);
        TRY_OR_FAIL(demuxer->create_context_for_track(track));
        while (true) {
            auto sample = demuxer->get_next_sample_for_track(track);
            if (sample.is_error())
                break;
            samples.append({ sample.value().presentation_timestamp(), sample.value().is_keyframe() });
        }
    }

    auto [inner_demuxer, track] = demuxer_and_video_track_for("./big_buck_bunny_5s.webm"sv);
    auto demuxer = BlockingDemuxer::create(inner_demuxer, NumericLimits<size_t>::max());
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, track, AK::Duration::from_milliseconds(20)));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    auto woken = false;
    producer->set_wake_handler([&] { woken = true; });
    // A wake can still be queued on the shared event loop once this test returns.
    ScopeGuard stop_handling_wakes = [&] { producer->set_wake_handler({}); };
    producer->start();

    // Left idle with its frame storage full, the producer suspends and wakes the consumer. Peeking counts as activity,
    // so only peek when woken.
    auto time_limit = AK::Duration::from_seconds(5);
    auto deadline = MonotonicTime::now_coarse() + time_limit;
    while (true) {
        while (!woken) {
            if (MonotonicTime::now_coarse() >= deadline) {
                FAIL("Timed out waiting for the producer to suspend");
                return;
            }
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }
        woken = false;
        if (producer->peek().status == Media::PipelineStatus::Suspended)
            break;
    }

    // Seek into the last sample of the group of pictures that the demuxer is partway into. Aim for the middle of it, since
    // the millisecond timestamps let the frame before it claim its first moment.
    auto next_sample_index = demuxer->samples_served();
    if (next_sample_index >= samples.size() || samples[next_sample_index].is_keyframe) {
        FAIL(ByteString::formatted("The producer suspended at sample {}, which doesn't continue a group of pictures", next_sample_index));
        return;
    }
    auto target_index = next_sample_index;
    while (target_index + 1 < samples.size() && !samples[target_index + 1].is_keyframe)
        target_index++;
    VERIFY(target_index + 1 < samples.size());
    auto target_sample_start = samples[target_index].timestamp.to_microseconds();
    auto target_sample_end = samples[target_index + 1].timestamp.to_microseconds();
    auto target = AK::Duration::from_microseconds((target_sample_start + target_sample_end) / 2);

    auto forced_seek_count_before_resuming = demuxer->forced_seek_count();
    producer->seek(target);
    auto frame = take_frame_within_time_limit(*producer, loop, time_limit);
    if (frame == nullptr) {
        FAIL("Timed out waiting for the seek to resolve");
        return;
    }
    EXPECT_EQ(frame->timestamp(), samples[target_index].timestamp);
    EXPECT_EQ(demuxer->forced_seek_count(), forced_seek_count_before_resuming + 1);
}

// The frames a decoder produced before it lost its state are still good, so they stay available while decoding
// restarts from a keyframe, and decoding continues right after them.
TEST_CASE(frames_held_when_the_decoder_loses_its_state_stay_available)
{
    auto& loop = never_destroyed_event_loop();
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };

    auto sample_timestamps = presentation_timestamps_of_first_samples("./vp9_in_webm.webm"sv, 6);
    VERIFY(sample_timestamps.size() == 6);
    size_t const samples_before_the_loss = 3;
    withheld_outputs->lose_state_instead_of_handing_out_output(samples_before_the_loss);

    auto [inner_demuxer, track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
    auto demuxer = BlockingDemuxer::create(inner_demuxer, samples_before_the_loss);
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->start();
    AsynchronousOutputReleaser releaser { withheld_outputs };

    // The output that loses the state can't arrive until the demuxer has more to read.
    auto time_limit = AK::Duration::from_seconds(5);
    RefPtr<Media::VideoFrame> first_held_frame;
    auto start_time = MonotonicTime::now_coarse();
    while (MonotonicTime::now_coarse() - start_time < time_limit) {
        auto output = producer->peek();
        if (output.status == Media::PipelineStatus::HaveData) {
            first_held_frame = output.frame;
            break;
        }
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    if (first_held_frame == nullptr) {
        FAIL("Timed out waiting for the first frame");
        return;
    }
    demuxer->stop_blocking_reads();
    start_time = MonotonicTime::now_coarse();
    while (!withheld_outputs->state_was_lost()) {
        if (MonotonicTime::now_coarse() - start_time >= time_limit) {
            FAIL("Timed out waiting for the decoder to lose its state");
            return;
        }
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    for (size_t frame_index = 0; frame_index < sample_timestamps.size(); frame_index++) {
        auto frame = take_frame_within_time_limit(*producer, loop, time_limit);
        if (frame == nullptr) {
            FAIL(ByteString::formatted("Timed out waiting for frame {}", frame_index));
            return;
        }
        if (frame_index == 0)
            EXPECT(frame.ptr() == first_held_frame.ptr());
        EXPECT_EQ(frame->timestamp(), sample_timestamps[frame_index]);
    }
}

// When the decoder loses its state during a seek past the end, the seek is retried. Once the retry reaches the end
// without any output, the seek still has to clamp to the last frame.
TEST_CASE(a_seek_past_the_end_lands_on_the_last_frame_when_the_decoder_loses_its_state)
{
    auto& loop = never_destroyed_event_loop();
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };
    withheld_outputs->lose_state_at_end_of_stream();

    auto sample_timestamps = presentation_timestamps_of_first_samples("./vp9_in_webm.webm"sv, NumericLimits<size_t>::max());
    VERIFY(!sample_timestamps.is_empty());
    auto [demuxer, track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });
    producer->seek(sample_timestamps.last() + AK::Duration::from_seconds(10));
    producer->start();
    AsynchronousOutputReleaser releaser { withheld_outputs };

    auto time_limit = AK::Duration::from_seconds(5);
    auto deadline = MonotonicTime::now_coarse() + time_limit;
    auto output = producer->peek();
    while (output.status != Media::PipelineStatus::HaveData && output.status != Media::PipelineStatus::EndOfStream) {
        if (MonotonicTime::now_coarse() >= deadline) {
            FAIL("Timed out waiting for the seek past the end to resolve");
            return;
        }
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        output = producer->peek();
    }
    EXPECT(withheld_outputs->state_was_lost());
    EXPECT_EQ(output.status, Media::PipelineStatus::HaveData);
    if (output.frame != nullptr)
        EXPECT_EQ(output.frame->timestamp(), sample_timestamps.last());
}

// Leaves the producer holding the first frame while it restarts decoding after the decoder loses its state. The
// restart waits in the decoder until an output is released, which a seek can't interrupt.
static bool hold_the_first_frame_through_a_restart(WithheldOutputs& withheld_outputs, AK::Duration time_limit)
{
    withheld_outputs.lose_state_instead_of_handing_out_output(1);
    if (!withheld_outputs.wait_until_the_caller_waits_for_output(time_limit))
        return false;
    EXPECT(withheld_outputs.release_next());
    if (!withheld_outputs.wait_until_withheld_count_reaches(WithholdingVideoDecoder::MAXIMUM_WITHHELD_OUTPUTS, time_limit))
        return false;
    if (!withheld_outputs.wait_until_the_caller_waits_for_output(time_limit))
        return false;

    // The restart's keyframe seek flushes the decoder, which then fills back up.
    EXPECT(withheld_outputs.release_next());
    if (!withheld_outputs.wait_until_withheld_count_reaches(WithholdingVideoDecoder::MAXIMUM_WITHHELD_OUTPUTS, time_limit))
        return false;
    if (!withheld_outputs.wait_until_the_caller_waits_for_output(time_limit))
        return false;
    return withheld_outputs.state_was_lost();
}

TEST_CASE(a_seek_during_a_restart_lands_on_its_own_target)
{
    auto& loop = never_destroyed_event_loop();
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };

    auto sample_timestamps = presentation_timestamps_of_first_samples("./vp9_in_webm.webm"sv, 11);
    VERIFY(sample_timestamps.size() == 11);
    auto producer = create_started_producer(loop, "./vp9_in_webm.webm"sv);

    auto time_limit = AK::Duration::from_seconds(5);
    if (!hold_the_first_frame_through_a_restart(withheld_outputs, time_limit)) {
        FAIL("Timed out waiting for a restart to wait in the decoder");
        return;
    }
    auto output = producer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::HaveData);
    if (output.frame != nullptr)
        EXPECT_EQ(output.frame->timestamp(), sample_timestamps[0]);

    producer->seek(sample_timestamps[10]);
    AsynchronousOutputReleaser releaser { withheld_outputs };
    auto frame = take_frame_within_time_limit(*producer, loop, time_limit);
    if (frame == nullptr) {
        FAIL("Timed out waiting for the seek to resolve");
        return;
    }
    EXPECT_EQ(frame->timestamp(), sample_timestamps[10]);
}

// A seek that the main thread resolves from the held frames must not cancel a restart that the decoder still needs.
TEST_CASE(a_seek_resolved_from_held_frames_keeps_a_pending_restart)
{
    auto& loop = never_destroyed_event_loop();
    auto withheld_outputs = make_ref_counted<WithheldOutputs>();
    WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(withheld_outputs);
    ScopeGuard stop_withholding = [] { WithholdingVideoDecoder::set_withheld_outputs_for_new_decoders(nullptr); };

    auto sample_timestamps = presentation_timestamps_of_first_samples("./vp9_in_webm.webm"sv, 11);
    VERIFY(sample_timestamps.size() == 11);
    auto producer = create_started_producer(loop, "./vp9_in_webm.webm"sv);

    auto time_limit = AK::Duration::from_seconds(5);
    if (!hold_the_first_frame_through_a_restart(withheld_outputs, time_limit)) {
        FAIL("Timed out waiting for a restart to wait in the decoder");
        return;
    }

    // The first seek can't resolve from the held frames, so it's pending when the second one resolves from them.
    producer->seek(sample_timestamps[10]);
    producer->seek(sample_timestamps[0]);
    auto frame = take_frame_within_time_limit(*producer, loop, time_limit);
    if (frame == nullptr) {
        FAIL("Timed out waiting for the held frame");
        return;
    }
    EXPECT_EQ(frame->timestamp(), sample_timestamps[0]);

    AsynchronousOutputReleaser releaser { withheld_outputs };
    for (size_t frame_index = 1; frame_index < 4; frame_index++) {
        frame = take_frame_within_time_limit(*producer, loop, time_limit);
        if (frame == nullptr) {
            FAIL(ByteString::formatted("Timed out waiting for frame {}", frame_index));
            return;
        }
        EXPECT_EQ(frame->timestamp(), sample_timestamps[frame_index]);
    }
}

TEST_CASE(samples_that_follow_a_transient_end_of_stream_are_ignored)
{
    auto& loop = never_destroyed_event_loop();
    auto [inner_demuxer, track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);
    auto demuxer = ResumingDemuxer::create(inner_demuxer);
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, track));
    producer->set_error_handler([&](Media::DecoderError&& error) {
        FAIL(ByteString::formatted("An error occurred while decoding: {}", error.description()));
    });

    // A seek past the end reads the whole stream first, so the decoder is drained on the end of stream and the
    // demuxer has resumed by the time the seek settles on the last frame.
    producer->seek(AK::Duration::from_seconds(60));
    producer->start();

    auto time_limit = AK::Duration::from_seconds(10);
    auto frame = take_frame_within_time_limit(*producer, loop, time_limit);
    if (frame == nullptr) {
        FAIL("Timed out waiting for the last frame");
        return;
    }
    EXPECT(demuxer->resumed_sample_count() > 0);

    auto start_time = MonotonicTime::now_coarse();
    while (producer->peek().status != Media::PipelineStatus::EndOfStream) {
        if (MonotonicTime::now_coarse() - start_time >= time_limit) {
            FAIL("Timed out waiting for the end of the stream");
            return;
        }
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
}
