/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/GenericShorthands.h>
#include <AK/Math.h>
#include <LibMedia/Containers/WAV/WAVDemuxer.h>
#include <LibMediaCodecs/CodedFrame.h>

namespace Media::WAV {

// Packets hold whole sample frames up to this size, a few milliseconds of audio at common rates.
static constexpr size_t TARGET_PACKET_BYTE_COUNT = 4096;

bool WAVDemuxer::supports_container_mime_type(ContainerMimeType mime_type)
{
    return mime_type.container_id == ContainerID::WAV && mime_type.media_type == ContainerMediaType::Audio;
}

bool WAVDemuxer::supports_codec_in_container(ContainerID container_id, CodecID codec_id)
{
    return container_id == ContainerID::WAV && first_is_one_of(codec_id, CodecID::U8, CodecID::S16LE, CodecID::S24LE, CodecID::S32LE, CodecID::F32LE, CodecID::ALaw, CodecID::MuLaw);
}

static Track create_track(Reader const& reader)
{
    Track track { TrackType::Audio, 0, Track::Kind::Main, {}, {} };
    track.set_audio_data({ .sample_specification = Audio::SampleSpecification(reader.sample_format().sample_rate, reader.sample_format().channel_map) });
    track.set_parsed_codec(ParsedCodec { reader.sample_format().codec_id });
    return track;
}

DecoderErrorOr<NonnullRefPtr<Demuxer>> WAVDemuxer::from_stream(NonnullRefPtr<MediaStream> const& stream)
{
    auto reader = TRY(Reader::from_stream(stream->create_cursor()));
    auto demuxer = make_ref_counted<WAVDemuxer>(stream, reader, create_track(reader));
    demuxer->start_buffered_scan_thread();
    return demuxer;
}

WAVDemuxer::WAVDemuxer(NonnullRefPtr<MediaStream> const& stream, Reader reader, Track track)
    : m_stream(stream)
    , m_reader(reader)
    , m_track(move(track))
{
    auto bytes_per_frame = m_reader.sample_format().bytes_per_frame;
    m_packet_byte_count = max<size_t>(bytes_per_frame, TARGET_PACKET_BYTE_COUNT / bytes_per_frame * bytes_per_frame);
}

WAVDemuxer::~WAVDemuxer()
{
    if (m_buffered_scan_thread != nullptr)
        m_buffered_scan_thread->shutdown();
}

static BufferedRangesScan buffered_time_ranges(Reader const& reader, Vector<MediaStream::ByteRange> const& byte_ranges)
{
    u64 sample_data_start = reader.sample_data_offset();
    auto sample_data_end = reader.sample_data_end().value_or(NumericLimits<u64>::max());
    auto time_at = [&](u64 position) {
        return AK::Duration::from_time_units(AK::clamp_to<i64>(position - sample_data_start), 1, reader.sample_format().bytes_per_second);
    };

    BufferedRangesScan scan;
    for (auto const& byte_range : byte_ranges) {
        auto start = max(byte_range.start, sample_data_start);
        auto end = min(byte_range.end, sample_data_end);
        if (start < end)
            scan.time_ranges.add_range(time_at(start), time_at(end));
    }
    if (!byte_ranges.is_empty())
        scan.last_byte_range_has_samples = byte_ranges.last().end > sample_data_start && byte_ranges.last().start < sample_data_end;
    return scan;
}

void WAVDemuxer::start_buffered_scan_thread()
{
    DemuxerScanState initial_state;
    initial_state.duration = m_reader.duration();

    m_buffered_scan_thread = DemuxerScanThread<Reader>::start(m_stream, move(initial_state), m_reader,
        [](MediaStream::Availability const& availability, Reader& reader) {
            return DemuxerScanState::create_from_track_scans({ buffered_time_ranges(reader, availability.byte_ranges) }, reader.duration(), availability.closing_bytes_are_available());
        });
}

WAVDemuxer::TrackStatus& WAVDemuxer::track_status()
{
    MutexLocker locker { m_track_status_mutex };
    VERIFY(m_track_status.has_value());
    return *m_track_status;
}

DecoderErrorOr<void> WAVDemuxer::create_context_for_track(Track const& track)
{
    VERIFY(track == m_track);

    auto cursor = m_stream->create_cursor();
    cursor->set_is_blocking(true);

    MutexLocker locker { m_track_status_mutex };
    VERIFY(!m_track_status.has_value());
    m_track_status = TrackStatus { move(cursor), m_reader.sample_data_offset(), true };
    return {};
}

DecoderErrorOr<Vector<Track>> WAVDemuxer::get_tracks_for_type(TrackType type)
{
    Vector<Track> tracks;
    if (type == TrackType::Audio)
        tracks.append(m_track);
    return tracks;
}

DecoderErrorOr<Optional<Track>> WAVDemuxer::get_preferred_track_for_type(TrackType type)
{
    if (type != TrackType::Audio)
        return Optional<Track> {};
    return Optional<Track> { m_track };
}

DecoderErrorOr<CodedFrame> WAVDemuxer::get_next_sample_for_track(Track const& track)
{
    VERIFY(track == m_track);
    auto& status = track_status();
    auto const& sample_format = m_reader.sample_format();

    u64 byte_count = m_packet_byte_count;
    if (auto sample_data_end = m_reader.sample_data_end(); sample_data_end.has_value()) {
        if (status.position >= *sample_data_end)
            return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of stream"sv);
        byte_count = min(byte_count, *sample_data_end - status.position);
    }
    auto frame_count = byte_count / sample_format.bytes_per_frame;
    if (frame_count == 0)
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of stream"sv);

    auto data = DECODER_TRY_ALLOC(FixedArray<u8>::create(frame_count * sample_format.bytes_per_frame));
    TRY(status.cursor->seek_to_position(status.position));
    auto read_byte_count = TRY(status.cursor->read_into(data.span()));
    // A stream that ends before its header said, or never said, delivers what it has in whole frames.
    auto read_frame_count = read_byte_count / sample_format.bytes_per_frame;
    if (read_frame_count == 0)
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of stream"sv);
    if (read_frame_count != frame_count)
        data = DECODER_TRY_ALLOC(FixedArray<u8>::create(data.span().trim(read_frame_count * sample_format.bytes_per_frame)));

    // A decoder needs no configuration beyond the frames themselves, but it does need one frame to be
    // marked as the start of a decode sequence.
    Optional<FixedArray<u8>> codec_configuration;
    if (status.needs_codec_configuration) {
        status.needs_codec_configuration = false;
        codec_configuration = FixedArray<u8> {};
    }

    auto first_frame_index = (status.position - m_reader.sample_data_offset()) / sample_format.bytes_per_frame;
    auto timestamp = AK::Duration::from_time_units(AK::clamp_to<i64>(first_frame_index), 1, sample_format.sample_rate);
    auto duration = AK::Duration::from_time_units(AK::clamp_to<i64>(read_frame_count), 1, sample_format.sample_rate);
    status.position += read_frame_count * sample_format.bytes_per_frame;

    return CodedFrame(sample_format.codec_id, timestamp, timestamp, duration, FrameFlags::Keyframe, move(data), move(codec_configuration));
}

AK::Duration WAVDemuxer::select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode)
{
    return target;
}

DecoderErrorOr<DemuxerSeekResult> WAVDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions options)
{
    VERIFY(track == m_track);
    auto& status = track_status();
    auto const& sample_format = m_reader.sample_format();

    if (has_flag(options, DemuxerSeekOptions::NeedCodecConfiguration))
        status.needs_codec_configuration = true;

    Checked<u64> position = static_cast<u64>(max<i64>(0, timestamp.to_time_units(1, sample_format.sample_rate)));
    position *= sample_format.bytes_per_frame;
    position += m_reader.sample_data_offset();
    auto sample_data_end = m_reader.sample_data_end().value_or(NumericLimits<u64>::max());
    if (position.has_overflow() || position.value() > sample_data_end)
        status.position = sample_data_end;
    else
        status.position = position.value();
    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<AK::Duration> WAVDemuxer::total_duration()
{
    return m_reader.duration();
}

DecoderErrorOr<AK::Duration> WAVDemuxer::duration_of_track(Track const&)
{
    return total_duration();
}

DemuxerScanState const& WAVDemuxer::scan_state() const
{
    return m_buffered_scan_thread->main_thread_state();
}

void WAVDemuxer::set_scan_state_change_handler(ScanStateChangeHandler handler)
{
    m_buffered_scan_thread->set_change_handler([handler = move(handler)] {
        if (handler)
            handler(TimeRanges {});
    });
}

void WAVDemuxer::set_blocking_reads_aborted_for_track(Track const&)
{
    track_status().cursor->abort();
}

void WAVDemuxer::reset_blocking_reads_aborted_for_track(Track const&)
{
    track_status().cursor->reset_abort();
}

void WAVDemuxer::set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler handler)
{
    track_status().cursor->set_blocked_change_handler(move(handler));
}

}
