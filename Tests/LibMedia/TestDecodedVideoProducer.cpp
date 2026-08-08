/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <Tests/LibMedia/TestMediaCommon.h>

namespace {

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
    virtual void set_scan_state_change_handler(Function<void()>) override { }

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

struct DemuxerAndTrack {
    NonnullRefPtr<Media::Demuxer> demuxer;
    Media::Track track;
};

static DemuxerAndTrack demuxer_and_video_track_for(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST([&] -> Media::DecoderErrorOr<NonnullRefPtr<Media::Demuxer>> {
        auto matroska_result = Media::Matroska::MatroskaDemuxer::from_stream(stream);
        if (!matroska_result.is_error())
            return matroska_result.release_value();
        return Media::FFmpeg::FFmpegDemuxer::from_stream(stream);
    }());
    auto tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());
    return { move(demuxer), tracks[0] };
}

}

TEST_CASE(a_codec_change_drains_the_previous_decoder)
{
    auto& loop = never_destroyed_event_loop();

    auto [avc_demuxer, avc_track] = demuxer_and_video_track_for("./avc.mp4"sv);
    auto [vp9_demuxer, vp9_track] = demuxer_and_video_track_for("./vp9_in_webm.webm"sv);

    auto switching_demuxer = SwitchingDemuxer::create(avc_demuxer, avc_track, vp9_demuxer, vp9_track);
    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, switching_demuxer, avc_track));
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
