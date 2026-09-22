/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Codecs/AAC.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/Containers/ADTS/ADTSDemuxer.h>
#include <LibMedia/Containers/FrameScanTimeline.h>
#include <LibMedia/SeekMode.h>

namespace Media::ADTS {

namespace {

class FrameScanSource final : public Media::FrameScanSource {
public:
    virtual Optional<Frame> next_frame(MediaStreamCursor& cursor, size_t position) const override
    {
        FrameIterator iterator { NonnullRefPtr { cursor }, position, FrameIterator::Resynchronize::Yes };
        auto frame = iterator.next();
        if (!frame.has_value())
            return {};

        return Frame {
            .position = frame->position,
            .byte_size = frame->header.frame_byte_size,
            .duration = { frame->header.sample_count, frame->header.sample_rate },
        };
    }

    virtual Optional<size_t> find_boundary_at_or_after(MediaStreamCursor& cursor, size_t start_byte, size_t upper_bound) const override
    {
        return Reader::find_frame_boundary_at_or_after(NonnullRefPtr { cursor }, start_byte, upper_bound);
    }

    virtual Optional<size_t> find_boundary_at_or_before(MediaStreamCursor& cursor, size_t target_byte, size_t lower_bound, size_t upper_bound) const override
    {
        return Reader::find_frame_boundary_at_or_before(NonnullRefPtr { cursor }, target_byte, lower_bound, upper_bound);
    }
};

}

bool ADTSDemuxer::supports_container_mime_type(ContainerMimeType mime_type)
{
    return mime_type.container_id == ContainerID::ADTS && mime_type.media_type == ContainerMediaType::Audio;
}

bool ADTSDemuxer::supports_codec_in_container(ContainerID container_id, CodecID codec_id)
{
    return container_id == ContainerID::ADTS && codec_id == CodecID::AAC;
}

static Audio::ChannelMap channel_map_from_channel_count(u8 channel_count)
{
    switch (channel_count) {
    case 1:
        return Audio::ChannelMap::mono();
    case 2:
        return Audio::ChannelMap::stereo();
    case 4:
        return Audio::ChannelMap::quadrophonic();
    case 6:
        return Audio::ChannelMap::surround_5_1();
    case 8:
        return Audio::ChannelMap::surround_7_1();
    default:
        return Audio::ChannelMap::invalid();
    }
}

static DecoderErrorOr<Track> create_track(Reader const& reader, ReadonlyBytes codec_configuration)
{
    auto const& header = reader.frame_header();

    Track track { TrackType::Audio, 0, Track::Kind::Main, {}, {} };
    track.set_audio_data({ .sample_specification = Audio::SampleSpecification(header.sample_rate, channel_map_from_channel_count(header.channel_count)) });

    auto parameters = Codecs::AAC::parse_configuration_record(codec_configuration, Codecs::AAC::MPEG4_AUDIO_OBJECT_TYPE_INDICATION);
    if (!parameters.has_value())
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "ADTS frames describe an unusable AAC configuration"sv);
    track.set_parsed_codec(ParsedCodec { *parameters });

    return track;
}

DecoderErrorOr<NonnullRefPtr<Demuxer>> ADTSDemuxer::from_stream(NonnullRefPtr<MediaStream> const& stream)
{
    auto reader = Reader::from_stream(stream->create_cursor());
    if (!reader.has_value())
        return DecoderError::with_description(DecoderErrorCategory::UnrecognizedFormat, "Found no ADTS frames in the stream"sv);

    auto const& header = reader->frame_header();

    // A stream carrying High Efficiency AAC names the sample rate of its core and leaves the rest to
    // be found in the audio itself, so the record says what the headers say and no more.
    auto codec_configuration = TRY(Codecs::AAC::create_configuration_record(header.audio_object_type, header.sample_rate, header.channel_count));
    auto track = TRY(create_track(*reader, codec_configuration.span()));

    auto demuxer = make_ref_counted<ADTSDemuxer>(stream, *reader, move(track), move(codec_configuration));
    demuxer->start_buffered_scan_thread();
    return demuxer;
}

ADTSDemuxer::ADTSDemuxer(NonnullRefPtr<MediaStream> const& stream, Reader reader, Track track, FixedArray<u8> codec_configuration)
    : m_stream(stream)
    , m_reader(reader)
    , m_track(move(track))
    , m_codec_configuration(move(codec_configuration))
    , m_seek_scanning_cursor(stream->create_cursor())
    , m_seek_cursor(stream->create_cursor())
{
    m_seek_scanning_cursor->set_is_blocking(false);
    m_seek_cursor->set_is_blocking(true);
}

ADTSDemuxer::~ADTSDemuxer()
{
    if (m_buffered_scan_thread != nullptr)
        m_buffered_scan_thread->shutdown();
}

void ADTSDemuxer::start_buffered_scan_thread()
{
    auto scan_cursor = m_stream->create_cursor();
    scan_cursor->set_is_blocking(false);

    Vector<Track> tracks;
    tracks.append(m_track);

    DemuxerScanState initial_state;
    initial_state.duration = m_reader.duration();

    m_buffered_scan_thread = DemuxerScanThread<BufferedScanPayload>::start(m_stream, move(initial_state),
        BufferedScanPayload {
            .timeline = make<FrameScanTimeline>(make<FrameScanSource>(), m_reader.first_frame_position(), m_reader.duration(), DurationSource::Estimated, OptionalNone {}),
            .scan_cursor = move(scan_cursor),
            .tracks = move(tracks),
        },
        [](MediaStream& stream, BufferedScanPayload& payload) {
            auto byte_ranges = stream.available_byte_ranges();
            HashMap<u64, BufferedRangesScan> scans_by_track_identifier;
            if (!byte_ranges.is_empty())
                scans_by_track_identifier.set(payload.tracks[0].identifier(), payload.timeline->buffered_time_ranges(payload.scan_cursor, byte_ranges, stream.expected_size()));
            return DemuxerScanState::create_from_track_scans(payload.tracks, move(scans_by_track_identifier), payload.timeline->duration(), stream.closing_bytes_are_available());
        });
}

ADTSDemuxer::TrackStatus& ADTSDemuxer::track_status()
{
    MutexLocker locker { m_track_status_mutex };
    VERIFY(m_track_status.has_value());
    return *m_track_status;
}

DecoderErrorOr<void> ADTSDemuxer::create_context_for_track(Track const& track)
{
    VERIFY(track == m_track);

    auto cursor = m_stream->create_cursor();
    cursor->set_is_blocking(true);

    MutexLocker locker { m_track_status_mutex };
    VERIFY(!m_track_status.has_value());
    m_track_status = TrackStatus { FrameIterator { move(cursor), m_reader.first_frame_position(), FrameIterator::Resynchronize::Yes }, AK::Duration::zero(), true };
    return {};
}

DecoderErrorOr<Vector<Track>> ADTSDemuxer::get_tracks_for_type(TrackType type)
{
    Vector<Track> tracks;
    if (type == TrackType::Audio)
        tracks.append(m_track);
    return tracks;
}

DecoderErrorOr<Optional<Track>> ADTSDemuxer::get_preferred_track_for_type(TrackType type)
{
    if (type != TrackType::Audio)
        return Optional<Track> {};
    return Optional<Track> { m_track };
}

DecoderErrorOr<CodedFrame> ADTSDemuxer::get_next_sample_for_track(Track const& track)
{
    VERIFY(track == m_track);
    auto& status = track_status();

    auto frame = status.iterator.next();
    if (!frame.has_value()) {
        if (status.iterator.cursor().is_aborted())
            return DecoderError::with_description(DecoderErrorCategory::Aborted, "Read aborted"sv);
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of stream"sv);
    }

    auto data = TRY(status.iterator.read_frame_data(*frame));
    auto duration = AK::Duration::from_time_units(frame->header.sample_count, 1, frame->header.sample_rate);
    auto timestamp = status.next_timestamp;
    status.next_timestamp += duration;

    Optional<FixedArray<u8>> codec_configuration;
    if (status.needs_codec_configuration) {
        status.needs_codec_configuration = false;
        codec_configuration = DECODER_TRY_ALLOC(FixedArray<u8>::create(m_codec_configuration.span()));
    }

    // Every frame can begin decoding, so every frame is a keyframe to seek to.
    return CodedFrame(CodecID::AAC, timestamp, timestamp, duration, FrameFlags::Keyframe, move(data), move(codec_configuration));
}

AK::Duration ADTSDemuxer::select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode)
{
    return target;
}

DecoderErrorOr<DemuxerSeekResult> ADTSDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions options)
{
    VERIFY(track == m_track);
    auto& status = track_status();

    if (has_flag(options, DemuxerSeekOptions::NeedCodecConfiguration))
        status.needs_codec_configuration = true;

    auto seek_result = TRY(m_buffered_scan_thread->payload().timeline->seek_to_timestamp(m_seek_scanning_cursor, m_seek_cursor, *m_stream, timestamp));
    auto const* seeked = seek_result.get_pointer<SeekedPosition>();
    if (seeked == nullptr)
        return DemuxerSeekResult::KeptCurrentPosition;

    status.iterator = FrameIterator { NonnullRefPtr { status.iterator.cursor() }, static_cast<size_t>(seeked->byte_position), FrameIterator::Resynchronize::Yes };
    status.next_timestamp = seeked->timestamp;
    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<AK::Duration> ADTSDemuxer::total_duration()
{
    return m_reader.duration();
}

DecoderErrorOr<AK::Duration> ADTSDemuxer::duration_of_track(Track const&)
{
    return total_duration();
}

DemuxerScanState const& ADTSDemuxer::scan_state() const
{
    return m_buffered_scan_thread->main_thread_state();
}

void ADTSDemuxer::set_scan_state_change_handler(Function<void()> handler)
{
    m_buffered_scan_thread->set_change_handler(move(handler));
}

void ADTSDemuxer::set_blocking_reads_aborted_for_track(Track const&)
{
    track_status().iterator.cursor().abort();
}

void ADTSDemuxer::reset_blocking_reads_aborted_for_track(Track const&)
{
    track_status().iterator.cursor().reset_abort();
}

void ADTSDemuxer::set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler handler)
{
    track_status().iterator.cursor().set_blocked_change_handler(move(handler));
}

}
