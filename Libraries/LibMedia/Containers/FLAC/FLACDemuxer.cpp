/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/Math.h>
#include <LibMedia/Codecs/FLAC.h>
#include <LibMedia/Containers/FLAC/FLACDemuxer.h>
#include <LibMediaCodecs/CodedFrame.h>

namespace Media::FLAC {

// Probing is capped so that a stream whose frames defy every estimate still gets walked to its target.
static constexpr size_t MAXIMUM_SEEK_PROBE_COUNT = 16;
// A scan looks for a frame within this distance of a range's ends, which bounds the work a range of junk costs on
// every change to the buffered data. Any frame of the format heads within it.
static constexpr size_t SCAN_WINDOW_BYTE_COUNT = 4 * MiB;

bool FLACDemuxer::supports_container_mime_type(ContainerMimeType mime_type)
{
    return mime_type.container_id == ContainerID::FLAC && mime_type.media_type == ContainerMediaType::Audio;
}

bool FLACDemuxer::supports_codec_in_container(ContainerID container_id, CodecID codec_id)
{
    return container_id == ContainerID::FLAC && codec_id == CodecID::FLAC;
}

static Track create_track(Reader const& reader)
{
    auto const& stream_info = reader.stream_info();
    Track track { TrackType::Audio, 0, Track::Kind::Main, {}, {} };
    track.set_audio_data({ .sample_specification = Audio::SampleSpecification(stream_info.sample_rate, Codecs::FLAC::channel_map(stream_info.channel_count)) });
    track.set_parsed_codec(ParsedCodec { CodecID::FLAC });
    return track;
}

DecoderErrorOr<NonnullRefPtr<Demuxer>> FLACDemuxer::from_stream(NonnullRefPtr<MediaStream> const& stream)
{
    auto reader = TRY(Reader::from_stream(stream->create_cursor()));
    auto demuxer = make_ref_counted<FLACDemuxer>(stream, reader, create_track(reader));
    demuxer->start_buffered_scan_thread();
    return demuxer;
}

FLACDemuxer::FLACDemuxer(NonnullRefPtr<MediaStream> const& stream, Reader reader, Track track)
    : m_stream(stream)
    , m_reader(move(reader))
    , m_track(move(track))
{
}

FLACDemuxer::~FLACDemuxer()
{
    if (m_buffered_scan_thread != nullptr)
        m_buffered_scan_thread->shutdown();
}

void FLACDemuxer::start_buffered_scan_thread()
{
    auto scan_cursor = m_stream->create_cursor();
    scan_cursor->set_is_blocking(false);

    DemuxerScanState initial_state;
    initial_state.duration = m_reader.duration();

    m_buffered_scan_thread = DemuxerScanThread<BufferedScanPayload>::start(m_stream, move(initial_state),
        BufferedScanPayload {
            .scanner = FrameIterator { move(scan_cursor), m_reader.frame_sync(), m_reader.first_frame_position(), FrameIterator::Resynchronize::Yes },
            .scanned_ranges = {},
            .anchors = m_scanned_anchors,
        },
        [reader = m_reader](MediaStream::Availability const& availability, BufferedScanPayload& payload) {
            auto const& stream_info = reader.stream_info();
            payload.scanned_ranges.update(availability.byte_ranges, [&](ScannedByteRange& range, CachedRangeChange change) {
                auto scan_start = max(range.byte_start, reader.first_frame_position());
                auto reaches_stream_end = availability.expected_size.has_value() && range.byte_end >= *availability.expected_size;
                if (has_flag(change, CachedRangeChange::Start) || !range.first_frame.has_value()) {
                    range.first_frame = {};
                    payload.scanner.set_position(scan_start);
                    auto resynced = payload.scanner.resync_forward(min(range.byte_end, scan_start + SCAN_WINDOW_BYTE_COUNT));
                    if (!resynced.is_error() && resynced.value()) {
                        if (auto frame = payload.scanner.read_next_frame(); !frame.is_error())
                            range.first_frame = SeekAnchor { frame.value().header.position, frame.value().header.sample_number };
                    }
                }
                if (has_flag(change, CachedRangeChange::End) || !range.end.has_value()) {
                    range.end = {};
                    auto search_start = max(scan_start, range.byte_end > SCAN_WINDOW_BYTE_COUNT ? range.byte_end - SCAN_WINDOW_BYTE_COUNT : 0);
                    // The last frame of a range is complete only when the stream ends with it.
                    if (auto header = payload.scanner.find_last_header_before(range.byte_end, search_start); header.has_value()) {
                        if (reaches_stream_end)
                            range.end = SeekAnchor { range.byte_end, header->sample_number + header->block_size };
                        else
                            range.end = SeekAnchor { header->position, header->sample_number };
                    }
                    // A sample past the stream's last is a false sync code.
                    if (stream_info.total_sample_count != 0 && range.end.has_value() && range.end->sample > stream_info.total_sample_count)
                        range.end = {};
                }
            });

            auto time_of = [&](u64 sample) { return AK::Duration::from_time_units(AK::clamp_to<i64>(sample), 1, stream_info.sample_rate); };
            BufferedRangesScan scan;
            Vector<SeekAnchor> anchors;
            for (auto const& range : payload.scanned_ranges.ranges()) {
                auto has_samples = range.first_frame.has_value() && range.end.has_value() && range.first_frame->sample < range.end->sample;
                scan.last_byte_range_has_samples = has_samples;
                if (!has_samples)
                    continue;
                scan.time_ranges.add_range(time_of(range.first_frame->sample), time_of(range.end->sample));
                anchors.append(*range.first_frame);
                anchors.append(*range.end);
            }
            {
                MutexLocker locker { payload.anchors->mutex };
                payload.anchors->anchors = move(anchors);
            }
            return DemuxerScanState::create_from_track_scans({ move(scan) }, reader.duration(), availability.closing_bytes_are_available());
        });
}

FLACDemuxer::TrackStatus& FLACDemuxer::track_status()
{
    MutexLocker locker { m_track_status_mutex };
    VERIFY(m_track_status.has_value());
    return *m_track_status;
}

DecoderErrorOr<void> FLACDemuxer::create_context_for_track(Track const& track)
{
    VERIFY(track == m_track);

    auto cursor = m_stream->create_cursor();
    cursor->set_is_blocking(true);

    MutexLocker locker { m_track_status_mutex };
    VERIFY(!m_track_status.has_value());
    m_track_status = TrackStatus { FrameIterator { move(cursor), m_reader.frame_sync(), m_reader.first_frame_position(), FrameIterator::Resynchronize::Yes }, true };
    return {};
}

DecoderErrorOr<Vector<Track>> FLACDemuxer::get_tracks_for_type(TrackType type)
{
    Vector<Track> tracks;
    if (type == TrackType::Audio)
        tracks.append(m_track);
    return tracks;
}

DecoderErrorOr<Optional<Track>> FLACDemuxer::get_preferred_track_for_type(TrackType type)
{
    if (type != TrackType::Audio)
        return Optional<Track> {};
    return Optional<Track> { m_track };
}

DecoderErrorOr<CodedFrame> FLACDemuxer::get_next_sample_for_track(Track const& track)
{
    VERIFY(track == m_track);
    auto& status = track_status();
    auto sample_rate = m_reader.stream_info().sample_rate;

    auto frame = TRY(status.iterator.read_next_frame());

    Optional<FixedArray<u8>> codec_configuration;
    if (status.needs_codec_configuration) {
        status.needs_codec_configuration = false;
        codec_configuration = DECODER_TRY_ALLOC(FixedArray<u8>::create(m_reader.codec_initialization_data()));
    }

    auto timestamp = AK::Duration::from_time_units(AK::clamp_to<i64>(frame.header.sample_number), 1, sample_rate);
    auto duration = AK::Duration::from_time_units(frame.header.block_size, 1, sample_rate);
    return CodedFrame(CodecID::FLAC, timestamp, timestamp, duration, FrameFlags::Keyframe, move(frame.data), move(codec_configuration));
}

AK::Duration FLACDemuxer::select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode)
{
    return target;
}

static DecoderErrorOr<Optional<FrameIterator::Frame>> read_frame_at_or_after(FrameIterator const& iterator, size_t position)
{
    FrameIterator probe = iterator;
    probe.set_position(position);
    if (!TRY(probe.resync_forward()))
        return Optional<FrameIterator::Frame> {};
    auto frame = probe.read_next_frame();
    if (frame.is_error()) {
        if (frame.error().category() == DecoderErrorCategory::EndOfStream)
            return Optional<FrameIterator::Frame> {};
        return frame.release_error();
    }
    return frame.release_value();
}

DecoderErrorOr<size_t> FLACDemuxer::find_frame_position_for_sample(FrameIterator& iterator, u64 target_sample)
{
    auto const& stream_info = m_reader.stream_info();
    auto stream_size = m_stream->expected_size();

    // The anchors are the frames known to lie closest to the target on either side.
    SeekAnchor low { m_reader.first_frame_position(), 0 };
    Optional<SeekAnchor> high;
    auto consider = [&](SeekAnchor anchor) {
        if (stream_size.has_value() && anchor.byte > *stream_size)
            return;
        if (anchor.sample <= target_sample) {
            if (anchor.sample >= low.sample)
                low = anchor;
        } else if (!high.has_value() || anchor.sample < high->sample) {
            high = anchor;
        }
    };
    if (stream_size.has_value() && stream_info.total_sample_count != 0)
        consider({ static_cast<size_t>(*stream_size), stream_info.total_sample_count });
    for (auto const& point : m_reader.seek_points()) {
        Checked<size_t> byte = m_reader.first_frame_position();
        byte += point.byte_offset;
        if (!byte.has_overflow())
            consider({ byte.value(), point.sample_number });
    }
    {
        MutexLocker locker { m_scanned_anchors->mutex };
        for (auto const& anchor : m_scanned_anchors->anchors)
            consider(anchor);
    }
    if (high.has_value() && high->byte <= low.byte)
        high = {};

    u64 underestimate_target_by = stream_info.sample_rate / 2;
    Optional<FrameIterator::Frame> overshot_frame;
    for (size_t probe = 0; probe < MAXIMUM_SEEK_PROBE_COUNT && target_sample - low.sample > underestimate_target_by; probe++) {
        if (high.has_value() && high->byte <= low.byte + 1)
            break;
        auto aim = target_sample - underestimate_target_by;
        size_t guess;
        if (overshot_frame.has_value()) {
            // If we overshot the target, jump back based on the found frame's bitrate.
            auto const& header = overshot_frame->header;
            auto bytes_per_sample = static_cast<double>(overshot_frame->data.size()) / header.block_size;
            auto step_back = static_cast<size_t>(static_cast<double>(header.sample_number - aim) * bytes_per_sample);
            guess = header.position > step_back ? header.position - step_back : 0;
        } else if (high.has_value() && high->sample > low.sample) {
            auto sample_fraction = static_cast<double>(aim - low.sample) / static_cast<double>(high->sample - low.sample);
            guess = low.byte + static_cast<size_t>(static_cast<double>(high->byte - low.byte) * sample_fraction);
        } else {
            break;
        }
        guess = clamp(guess, low.byte + 1, high.has_value() ? high->byte - 1 : NumericLimits<size_t>::max());

        auto frame = TRY(read_frame_at_or_after(iterator, guess));
        if (!frame.has_value())
            break;

        auto const& header = frame->header;
        if (header.sample_number <= target_sample && target_sample < header.sample_number + header.block_size)
            return header.position;
        if (header.sample_number < target_sample) {
            low = SeekAnchor { frame->end_position(), header.sample_number + header.block_size };
            overshot_frame = {};
        } else {
            high = SeekAnchor { header.position, header.sample_number };
            overshot_frame = frame.release_value();
            underestimate_target_by *= 2;
        }
    }

    FrameIterator walker = iterator;
    walker.set_position(low.byte);
    while (true) {
        auto frame = walker.read_next_frame();
        if (frame.is_error()) {
            if (frame.error().category() == DecoderErrorCategory::EndOfStream)
                return walker.position();
            return frame.release_error();
        }
        if (frame.value().header.sample_number + frame.value().header.block_size > target_sample)
            return frame.value().header.position;
    }
}

DecoderErrorOr<DemuxerSeekResult> FLACDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions options)
{
    VERIFY(track == m_track);
    auto& status = track_status();

    if (has_flag(options, DemuxerSeekOptions::NeedCodecConfiguration))
        status.needs_codec_configuration = true;

    auto target_sample = static_cast<u64>(max<i64>(0, timestamp.to_time_units(1, m_reader.stream_info().sample_rate)));
    status.iterator.set_position(TRY(find_frame_position_for_sample(status.iterator, target_sample)));
    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<AK::Duration> FLACDemuxer::total_duration()
{
    return m_reader.duration();
}

DecoderErrorOr<AK::Duration> FLACDemuxer::duration_of_track(Track const&)
{
    return total_duration();
}

DemuxerScanState const& FLACDemuxer::scan_state() const
{
    return m_buffered_scan_thread->main_thread_state();
}

void FLACDemuxer::set_scan_state_change_handler(ScanStateChangeHandler handler)
{
    m_buffered_scan_thread->set_change_handler([handler = move(handler)] {
        if (handler)
            handler(TimeRanges {});
    });
}

void FLACDemuxer::set_blocking_reads_aborted_for_track(Track const&)
{
    track_status().iterator.cursor().abort();
}

void FLACDemuxer::reset_blocking_reads_aborted_for_track(Track const&)
{
    track_status().iterator.cursor().reset_abort();
}

void FLACDemuxer::set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler handler)
{
    track_status().iterator.cursor().set_blocked_change_handler(move(handler));
}

}
