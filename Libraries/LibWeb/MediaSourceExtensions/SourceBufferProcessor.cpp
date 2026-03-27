/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Math.h>
#include <AK/NonnullOwnPtr.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/ReadonlyBytesCursor.h>
#include <LibWeb/MediaSourceExtensions/ByteStreamParser.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferProcessor.h>
#include <LibWeb/MediaSourceExtensions/TrackBuffer.h>
#include <LibWeb/MediaSourceExtensions/TrackBufferDemuxer.h>

namespace Web::MediaSourceExtensions {

SourceBufferProcessor::SourceBufferProcessor()
    : m_cursor(adopt_ref(*new Media::ReadonlyBytesCursor({})))
{
}
SourceBufferProcessor::~SourceBufferProcessor() = default;

void SourceBufferProcessor::set_parser(NonnullOwnPtr<ByteStreamParser>&& parser)
{
    m_parser = move(parser);
}

AppendMode SourceBufferProcessor::mode() const
{
    return m_mode;
}

bool SourceBufferProcessor::is_parsing_media_segment() const
{
    return m_append_state == AppendState::ParsingMediaSegment;
}

bool SourceBufferProcessor::generate_timestamps_flag() const
{
    return m_generate_timestamps_flag;
}

AK::Duration SourceBufferProcessor::group_end_timestamp() const
{
    return m_group_end_timestamp;
}

bool SourceBufferProcessor::is_buffer_full() const
{
    return m_buffer_full_flag;
}

void SourceBufferProcessor::set_mode(AppendMode mode)
{
    m_mode = mode;
}

void SourceBufferProcessor::set_generate_timestamps_flag(bool flag)
{
    m_generate_timestamps_flag = flag;
}

void SourceBufferProcessor::set_group_start_timestamp(Optional<AK::Duration> timestamp)
{
    m_group_start_timestamp = timestamp;
}

bool SourceBufferProcessor::first_initialization_segment_received_flag() const
{
    return m_first_initialization_segment_received_flag;
}

void SourceBufferProcessor::set_first_initialization_segment_received_flag(bool flag)
{
    m_first_initialization_segment_received_flag = flag;
}

void SourceBufferProcessor::set_pending_initialization_segment_for_change_type_flag(bool flag)
{
    m_pending_initialization_segment_for_change_type_flag = flag;
}

void SourceBufferProcessor::set_duration_change_callback(DurationChangeCallback callback)
{
    m_duration_change_callback = move(callback);
}

void SourceBufferProcessor::set_first_initialization_segment_callback(InitializationSegmentCallback callback)
{
    m_first_initialization_segment_callback = move(callback);
}

void SourceBufferProcessor::set_append_error_callback(AppendErrorCallback callback)
{
    m_append_error_callback = move(callback);
}

void SourceBufferProcessor::set_coded_frame_processing_done_callback(CodedFrameProcessingDoneCallback callback)
{
    m_coded_frame_processing_done_callback = move(callback);
}

void SourceBufferProcessor::set_append_done_callback(AppendDoneCallback callback)
{
    m_append_done_callback = move(callback);
}

void SourceBufferProcessor::append_to_input_buffer(ReadonlyBytes bytes)
{
    m_input_buffer.append(bytes);
    m_cursor->set_data(m_input_buffer.bytes());
}

// https://w3c.github.io/media-source/#sourcebuffer-segment-parser-loop
void SourceBufferProcessor::run_segment_parser_loop()
{
    VERIFY(m_parser);

    while (true) {
        // 1. Loop Top: If the [[input buffer]] is empty, then jump to the need more data step below.
        if (m_cursor->position() >= m_cursor->size())
            goto need_more_data;

        // 2. If the [[input buffer]] contains bytes that violate the SourceBuffer byte stream format specification,
        //    then run the append error algorithm and abort this algorithm.
        // AD-HOC: We'll react to this below when actually parsing the segments.

        // 3. Remove any bytes that the byte stream format specifications say MUST be ignored from the start of
        //    the [[input buffer]].
        {
            auto skip_result = m_parser->skip_ignored_bytes(*m_cursor);
            if (skip_result.is_error()) {
                if (skip_result.error().category() == Media::DecoderErrorCategory::EndOfStream)
                    goto need_more_data;
                m_append_error_callback();
                return;
            }
            drop_consumed_bytes_from_input_buffer();
        }

        // 4. If the [[append state]] equals WAITING_FOR_SEGMENT, then run the following steps:
        if (m_append_state == AppendState::WaitingForSegment) {
            auto sniff_result = m_parser->sniff_segment_type(*m_cursor);
            if (sniff_result.is_error()) {
                m_append_error_callback();
                return;
            }
            auto segment_type = sniff_result.value();

            // 1. If the beginning of the [[input buffer]] indicates the start of an initialization segment, set the
            //    [[append state]] to PARSING_INIT_SEGMENT.
            if (segment_type == SegmentType::InitializationSegment) {
                m_append_state = AppendState::ParsingInitSegment;
                // 2. If the beginning of the [[input buffer]] indicates the start of a media segment, set [[append
                //    state]] to PARSING_MEDIA_SEGMENT.
            } else if (segment_type == SegmentType::MediaSegment) {
                m_append_state = AppendState::ParsingMediaSegment;
            } else if (segment_type == SegmentType::Incomplete) {
                // NB: If we cannot determine the type due to an incomplete segment, this is equivalent to if we were
                //     parsing an initialization segment and didn't have enough data, which would result in jumping to
                //     the need more data step.
                goto need_more_data;
            } else {
                VERIFY(segment_type == SegmentType::Unknown);
                m_append_error_callback();
                return;
            }

            // 3. Jump to the loop top step above.
            continue;
        }

        // 5. If the [[append state]] equals PARSING_INIT_SEGMENT, then run the following steps:
        if (m_append_state == AppendState::ParsingInitSegment) {
            // 1. If the [[input buffer]] does not contain a complete initialization segment yet, then jump to the need
            //    more data step below.
            auto parse_result = m_parser->parse_initialization_segment(*m_cursor);
            if (parse_result.is_error()) {
                if (parse_result.error().category() == Media::DecoderErrorCategory::EndOfStream)
                    goto need_more_data;

                // AD-HOC: Handle bytes that violate the byte stream format specification as specified above.
                m_append_error_callback();
                return;
            }

            // 2. Run the initialization segment received algorithm.
            initialization_segment_received();

            // 3. Remove the initialization segment bytes from the beginning of the [[input buffer]].
            drop_consumed_bytes_from_input_buffer();

            // 4. Set [[append state]] to WAITING_FOR_SEGMENT.
            m_append_state = AppendState::WaitingForSegment;

            // 5. Jump to the loop top step above.
            continue;
        }

        // 6. If the [[append state]] equals PARSING_MEDIA_SEGMENT, then run the following steps:
        if (m_append_state == AppendState::ParsingMediaSegment) {
            // 1. If the [[first initialization segment received flag]] is false or the [[pending initialization
            //    segment for changeType flag]] is true, then run the append error algorithm and abort this algorithm.
            if (!m_first_initialization_segment_received_flag || m_pending_initialization_segment_for_change_type_flag) {
                m_append_error_callback();
                return;
            }

            {
                // 2. If the [[input buffer]] contains one or more complete coded frames, then run the coded frame
                //    processing algorithm.
                auto parse_result = m_parser->parse_media_segment(*m_cursor);
                if (parse_result.is_error()) {
                    // AD-HOC: Handle bytes that violate the byte stream format specification as specified above.
                    m_append_error_callback();
                    return;
                }

                run_coded_frame_processing(parse_result.value().coded_frames);

                // FIXME: 3. If this SourceBuffer is full and cannot accept more media data, then set the [[buffer full flag]]
                //           to true.

                // 4. If the [[input buffer]] does not contain a complete media segment, then jump to the need more
                //    data step below.
                if (!parse_result.value().completed_segment)
                    goto need_more_data;
            }

            // 5. Remove the media segment bytes from the beginning of the [[input buffer]].
            drop_consumed_bytes_from_input_buffer();

            // 6. Set [[append state]] to WAITING_FOR_SEGMENT.
            m_append_state = AppendState::WaitingForSegment;

            // 7. Jump to the loop top step above.
            continue;
        }

        // 7. Need more data: Return control to the calling algorithm.
    need_more_data:
        drop_consumed_bytes_from_input_buffer();
        m_append_done_callback();
        return;
    }
}

// https://w3c.github.io/media-source/#sourcebuffer-reset-parser-state
void SourceBufferProcessor::reset_parser_state()
{
    // 1. If the [[append state]] equals PARSING_MEDIA_SEGMENT and the [[input buffer]] contains some
    //    complete coded frames, then run the coded frame processing algorithm until all of these
    //    complete coded frames have been processed.
    if (m_append_state == AppendState::ParsingMediaSegment) {
        // FIXME: Process any complete coded frames
    }

    // 2. Unset the last decode timestamp on all track buffers.
    // 3. Unset the last frame duration on all track buffers.
    // 4. Unset the highest end timestamp on all track buffers.
    unset_all_track_buffer_timestamps();

    // 5. Set the need random access point flag on all track buffers to true.
    set_need_random_access_point_flag_on_all_track_buffers(true);

    // 6. If the mode attribute equals "sequence", then
    if (m_mode == AppendMode::Sequence) {
        // set the [[group start timestamp]] to the [[group end timestamp]]
        m_group_start_timestamp = m_group_end_timestamp;
    }

    // 7. Remove all bytes from the [[input buffer]].
    m_input_buffer.clear();
    m_cursor->set_data({});
    MUST(m_cursor->seek(0, SeekMode::SetPosition));

    // 8. Set [[append state]] to WAITING_FOR_SEGMENT.
    m_append_state = AppendState::WaitingForSegment;
}

// https://w3c.github.io/media-source/#sourcebuffer-init-segment-received
void SourceBufferProcessor::initialization_segment_received()
{
    // 1. Update the duration attribute if it currently equals NaN:
    // AD-HOC: Pass off the duration to the callback, and allow it to check for NaN.
    {
        // If the initialization segment contains a duration:
        if (m_parser->duration().has_value()) {
            // Run the duration change algorithm with new duration set to the duration in the initialization segment.
            m_duration_change_callback(m_parser->duration().value().to_seconds_f64());
        }
        // Otherwise:
        else {
            // Run the duration change algorithm with new duration set to positive Infinity.
            m_duration_change_callback(AK::Infinity<double>);
        }
    }

    // 2. If the initialization segment has no audio, video, or text tracks, then run the append error algorithm
    //    and abort these steps.
    if (m_parser->video_tracks().is_empty() && m_parser->audio_tracks().is_empty() && m_parser->text_tracks().is_empty()) {
        m_append_error_callback();
        return;
    }

    // 3. If the [[first initialization segment received flag]] is true, then run the following steps:
    if (m_first_initialization_segment_received_flag) {
        // FIXME: 1. Verify the following properties. If any of the checks fail then run the append error algorithm
        //           and abort these steps.
        //               - The number of audio, video, and text tracks match what was in the first initialization segment.
        //               - If more than one track for a single type are present (e.g., 2 audio tracks), then the Track IDs
        //                 match the ones in the first initialization segment.
        //               - The codecs for each track are supported by the user agent.

        // FIXME: 2. Add the appropriate track descriptions from this initialization segment to each of the track buffers.

        // 3. Set the need random access point flag on all track buffers to true.
        set_need_random_access_point_flag_on_all_track_buffers(true);
    }

    // 4. Let active track flag equal false.
    // NB: active track flag is never true unless [[first initialization segment received flag]] is true, and it is
    //     used only by the synchronous code, so we handle this in the callback invoked below.

    // 5. If the [[first initialization segment received flag]] is false, then run the following steps:
    if (!m_first_initialization_segment_received_flag) {
        // FIXME: 1. If the initialization segment contains tracks with codecs the user agent does not support,
        //           then run the append error algorithm and abort these steps.

        auto build_tracks = [&](Vector<Media::Track> const& tracks) {
            Vector<InitializationSegmentTrack> result;
            // 2. For each audio track in the initialization segment, run following steps:
            // 3. For each video track in the initialization segment, run following steps:
            // 4. For each text track in the initialization segment, run following steps:
            for (auto const& track : tracks) {
                // AD-HOC: Steps 1-6 are handled in the callback invoked below.

                // 7. Create a new track buffer to store coded frames for this track.
                // 8. Add the track description for this track to the track buffer.
                auto codec_id = m_parser->codec_id_for_track(track.identifier());
                auto codec_init_data = MUST(ByteBuffer::copy(m_parser->codec_initialization_data_for_track(track.identifier())));
                auto demuxer = make_ref_counted<TrackBufferDemuxer>(track, codec_id, move(codec_init_data));
                auto track_buffer = make<TrackBuffer>(demuxer);
                m_track_buffers.set(track.identifier(), move(track_buffer));

                // AD-HOC: Pass off the track information to the callback so that it can initialize the DOM objects.
                result.append({ .track = track, .demuxer = demuxer });
            }
            return result;
        };

        m_first_initialization_segment_callback({
            .audio_tracks = build_tracks(m_parser->audio_tracks()),
            .video_tracks = build_tracks(m_parser->video_tracks()),
            .text_tracks = build_tracks(m_parser->text_tracks()),
        });

        // 6. Set [[first initialization segment received flag]] to true.
        m_first_initialization_segment_received_flag = true;
    }

    // 6. Set [[pending initialization segment for changeType flag]] to false.
    m_pending_initialization_segment_for_change_type_flag = false;

    // 7. If the active track flag equals true, then run the following steps:
    // NB: Steps 8-9 (updating the element's readyState) are handled by the initialization segment callback invoked
    //     above. Since active track flag is only true if the first initialization segment was being received, this
    //     will only need to happen when that callback is invoked, so we don't need separate one.
}

// https://w3c.github.io/media-source/#sourcebuffer-coded-frame-processing
void SourceBufferProcessor::run_coded_frame_processing(Vector<DemuxedCodedFrame>& coded_frames)
{
    // 1. For each coded frame in the media segment run the following steps:
    for (auto& demuxed_frame : coded_frames) {
        auto& frame = demuxed_frame.coded_frame;

        // 1. Loop Top:
    loop_top:

        // FIXME: If generate timestamps flag equals true:
        //            1. Let presentation timestamp equal 0.
        //            2. Let decode timestamp equal 0.
        //        Otherwise:
        //            1. Let presentation timestamp be a double precision floating point representation
        //               of the coded frame's presentation timestamp in seconds.
        //            2. Let decode timestamp be a double precision floating point representation
        //               of the coded frame's decode timestamp in seconds.
        auto presentation_timestamp = frame.timestamp();
        // FIXME: For VP9, decode timestamp equals presentation timestamp. This will need to differ when H.264 is
        //        supported by MSE.
        auto decode_timestamp = frame.timestamp();

        // 2. Let frame duration be a double precision floating point representation of the coded
        //    frame's duration in seconds.
        auto frame_duration = frame.duration();

        // FIXME: 3. If mode equals "sequence" and group start timestamp is set, then run the following steps:

        // FIXME: 4. If timestampOffset is not 0, then run the following steps:

        // 5. Let track buffer equal the track buffer that the coded frame will be added to.
        auto maybe_track_buffer = m_track_buffers.get(demuxed_frame.track_number);

        // AD-HOC: If we're passed a media segment containing coded frames from a track we don't know about, don't
        //         crash on it.
        if (!maybe_track_buffer.has_value())
            continue;
        auto& track_buffer = *maybe_track_buffer.release_value();
        auto& demuxer = track_buffer.demuxer();

        auto last_decode_timestamp = track_buffer.last_decode_timestamp();
        auto last_frame_duration = track_buffer.last_frame_duration();

        // 6.
        if (
            // -> If last decode timestamp for track buffer is set
            (last_decode_timestamp.has_value()
                //  and decode timestamp is less than last decode timestamp:
                && decode_timestamp < last_decode_timestamp.value())
            // OR
            ||
            // -> If last decode timestamp for track buffer is set
            (last_decode_timestamp.has_value()
                // and the difference between decode timestamp and last decode timestamp is greater than 2 times last frame duration:
                && decode_timestamp - last_decode_timestamp.value() > (last_frame_duration.value() + last_frame_duration.value()))) {
            // 1. -> If mode equals "segments":
            if (m_mode == AppendMode::Segments) {
                // Set [[group end timestamp]] to presentation timestamp.
                m_group_end_timestamp = presentation_timestamp;
            }
            //    -> If mode equals "sequence":
            if (m_mode == AppendMode::Sequence) {
                // -> Set [[group start timestamp]] equal to the [[group end timestamp]].
                m_group_start_timestamp = m_group_end_timestamp;
            }

            for (auto& [id, track_buffer] : m_track_buffers) {
                // 2. Unset the last decode timestamp on all track buffers.
                track_buffer->unset_last_decode_timestamp();

                // 3. Unset the last frame duration on all track buffers.
                track_buffer->unset_last_frame_duration();

                // 4. Unset the highest end timestamp on all track buffers.
                track_buffer->unset_highest_end_timestamp();

                // 5. Set the need random access point flag on all track buffers to true.
                track_buffer->set_need_random_access_point_flag(true);
            }
            // 6. Jump to the Loop Top step above to restart processing of the current coded frame.
            goto loop_top;
        }

        // 7. Let frame end timestamp equal the sum of presentation timestamp and frame duration.
        auto frame_end_timestamp = presentation_timestamp + frame_duration;

        // FIXME: 8. If presentation timestamp is less than appendWindowStart, then set the need random access
        //           point flag to true, drop the coded frame, and jump to the top of the loop.

        // FIXME: 9. If frame end timestamp is greater than appendWindowEnd, then set the need random access
        //           point flag to true, drop the coded frame, and jump to the top of the loop.

        // 10. If the need random access point flag on track buffer equals true, then run the following steps:
        if (track_buffer.need_random_access_point_flag()) {
            // 1. If the coded frame is not a random access point, then drop the coded frame and jump to
            //    the top of the loop.
            if (!frame.is_keyframe())
                continue;
            // 2. Set the need random access point flag on track buffer to false.
            track_buffer.set_need_random_access_point_flag(false);
        }

        // FIXME: 11. Let spliced audio frame be an unset variable for holding audio splice information

        // FIXME: 12. Let spliced timed text frame be an unset variable for holding timed text splice information

        // FIXME: 13. If last decode timestamp for track buffer is unset and presentation timestamp falls within
        //            the presentation interval of a coded frame in track buffer, then run the following steps:

        // 14. Remove all coded frames from track buffer that have a presentation timestamp greater than
        //     or equal to presentation timestamp and less than frame end timestamp.
        // 15. Remove all possible decoding dependencies on the coded frames removed in the previous step
        //     by removing all coded frames from track buffer between those frames removed in the previous
        //     step and the next random access point after those removed frames.
        demuxer.remove_coded_frames_and_dependants_in_range(presentation_timestamp, frame_end_timestamp);

        // 16. If spliced audio frame is set:
        //         Add spliced audio frame to the track buffer.
        //     If spliced timed text frame is set:
        //         Add spliced timed text frame to the track buffer.
        //     Otherwise:
        //         Add the coded frame with the presentation timestamp, decode timestamp, and frame
        //         duration to the track buffer.
        demuxer.add_coded_frame(move(frame));

        // 17. Set last decode timestamp for track buffer to decode timestamp.
        track_buffer.set_last_decode_timestamp(decode_timestamp);

        // 18. Set last frame duration for track buffer to frame duration.
        track_buffer.set_last_frame_duration(frame_duration);

        // 19. If highest end timestamp for track buffer is unset or frame end timestamp is greater
        //     than highest end timestamp, then
        if (!track_buffer.highest_end_timestamp().has_value()
            || frame_end_timestamp > track_buffer.highest_end_timestamp().value()) {
            // set highest end timestamp for track buffer to frame end timestamp.
            track_buffer.set_highest_end_timestamp(frame_end_timestamp);
        }

        // 20. If frame end timestamp is greater than group end timestamp, then set group end timestamp
        //     equal to frame end timestamp.
        if (frame_end_timestamp > m_group_end_timestamp)
            m_group_end_timestamp = frame_end_timestamp;

        // FIXME: 21. If generate timestamps flag equals true, then set timestampOffset equal to
        //            frame end timestamp.
    }

    // AD-HOC: Steps 2-5 are handled by the callback, as they mutate the DOM.
    m_coded_frame_processing_done_callback();
}

// https://w3c.github.io/media-source/#sourcebuffer-coded-frame-eviction
void SourceBufferProcessor::run_coded_frame_eviction()
{
    // FIXME: 1. Let new data equal the data that is about to be appended to this SourceBuffer.
    //        2. If the [[buffer full flag]] equals false, then abort these steps.
    //        3. Let removal ranges equal a list of presentation time ranges that can be evicted from the presentation
    //           to make room for the new data.
    //        4. For each range in removal ranges, run the coded frame removal algorithm with start and end equal to
    //           the removal range start and end timestamp respectively.
}

void SourceBufferProcessor::drop_consumed_bytes_from_input_buffer()
{
    auto consumed = m_cursor->position();
    if (consumed == 0)
        return;
    VERIFY(consumed <= m_input_buffer.size());
    auto remaining_bytes = m_input_buffer.bytes().slice(consumed);
    AK::TypedTransfer<u8>::move(m_input_buffer.data(), remaining_bytes.data(), remaining_bytes.size());
    m_input_buffer.trim(remaining_bytes.size(), false);
    m_cursor->set_data(m_input_buffer.bytes());
    MUST(m_cursor->seek(0, SeekMode::SetPosition));
}

void SourceBufferProcessor::unset_all_track_buffer_timestamps()
{
    for (auto& [track_id, track_buffer] : m_track_buffers) {
        track_buffer->unset_last_decode_timestamp();
        track_buffer->unset_last_frame_duration();
        track_buffer->unset_highest_end_timestamp();
    }
}

void SourceBufferProcessor::set_need_random_access_point_flag_on_all_track_buffers(bool flag)
{
    for (auto& [track_id, track_buffer] : m_track_buffers) {
        track_buffer->set_need_random_access_point_flag(flag);
    }
}

void SourceBufferProcessor::set_reached_end_of_stream()
{
    for (auto& [track_id, track_buffer] : m_track_buffers)
        track_buffer->demuxer().set_reached_end_of_stream();
}

void SourceBufferProcessor::clear_reached_end_of_stream()
{
    for (auto& [track_id, track_buffer] : m_track_buffers)
        track_buffer->demuxer().clear_reached_end_of_stream();
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-buffered
Media::TimeRanges SourceBufferProcessor::buffered_ranges() const
{
    // 2. Let highest end time be the largest track buffer ranges end time across all the track buffers
    //    managed by this SourceBuffer object.
    AK::Duration highest_end_time;
    for (auto const& [track_id, track_buffer] : m_track_buffers) {
        auto end_time = track_buffer->demuxer().track_buffer_ranges().highest_end_time();
        highest_end_time = max(highest_end_time, end_time);
    }

    // 3. Let intersection ranges equal a TimeRanges object containing a single range from 0 to highest end time.
    Media::TimeRanges intersection;
    if (highest_end_time > AK::Duration::zero())
        intersection.add_range(AK::Duration::zero(), highest_end_time);

    // 4. For each audio and video track buffer managed by this SourceBuffer, run the following steps:
    for (auto const& [track_id, track_buffer] : m_track_buffers) {
        // 1. Let track ranges equal the track buffer ranges for the current track buffer.
        auto track_ranges = track_buffer->demuxer().track_buffer_ranges();

        // 2. If readyState is "ended", then set the end time on the last range in track ranges to
        //    highest end time.
        // FIXME: Check readyState from the parent MediaSource.

        // 3. Let new intersection ranges equal the intersection between the intersection ranges and
        //    the track ranges.
        // 4. Replace the ranges in intersection ranges with the new intersection ranges.
        intersection = intersection.intersection(track_ranges);
    }

    return intersection;
}

}
