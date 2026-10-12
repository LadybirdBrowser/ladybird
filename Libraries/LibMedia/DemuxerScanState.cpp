/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMedia/DemuxerScanState.h>

namespace Media {

DemuxerScanState DemuxerScanState::create_from_track_scans(Vector<BufferedRangesScan> track_scans, AK::Duration minimum_duration, bool closing_bytes_are_available)
{
    DemuxerScanState state;
    state.duration = minimum_duration;
    state.reached_end_of_stream = closing_bytes_are_available && !track_scans.is_empty();
    for (auto& scan : track_scans) {
        state.duration = max(state.duration, scan.time_ranges.highest_end_time());
        if (!scan.last_byte_range_has_samples)
            state.reached_end_of_stream = false;
        state.track_buffered_ranges.append({ scan.track_identifier, move(scan.time_ranges), closing_bytes_are_available && scan.last_byte_range_has_samples });
    }
    return state;
}

TimeRanges DemuxerScanState::buffered_ranges() const
{
    return combined_scan_state().time_ranges;
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-buffered
CombinedScanState DemuxerScanState::combined_scan_state(Function<bool(u64)> const& is_track_enabled) const
{
    // NB: Files restrict this calculation to enabled tracks; SourceBuffers include all tracks.
    auto includes_track = [&](u64 identifier) {
        return buffered_ranges_policy == BufferedRangesPolicy::AllTracks || is_track_enabled(identifier);
    };
    CombinedScanState result;
    result.duration = duration;
    result.resource_is_complete = resource_is_complete;
    AK::Duration highest_end_time;
    bool has_tracks = false;
    bool all_tracks_ended = true;
    for (auto const& ranges : track_buffered_ranges) {
        if (!includes_track(ranges.track_identifier))
            continue;
        has_tracks = true;
        all_tracks_ended &= ranges.reached_end_of_stream;
        highest_end_time = max(highest_end_time, ranges.time_ranges.highest_end_time());
    }
    result.reached_end_of_stream = buffered_ranges_policy == BufferedRangesPolicy::AllTracks ? reached_end_of_stream : has_tracks && all_tracks_ended;

    TimeRanges intersection { { AK::Duration::zero(), highest_end_time } };
    for (auto const& ranges : track_buffered_ranges) {
        if (!includes_track(ranges.track_identifier))
            continue;
        auto track_ranges = ranges.time_ranges;

        // NB: AllTracks follows SourceBuffer's buffered attribute. File tracks can reach their ends independently.
        auto track_has_ended = buffered_ranges_policy == BufferedRangesPolicy::AllTracks ? reached_end_of_stream : ranges.reached_end_of_stream;
        if (track_has_ended && !track_ranges.is_empty())
            track_ranges.add_range(track_ranges[track_ranges.size() - 1].start, highest_end_time);
        intersection = intersection.intersection(track_ranges);
    }
    result.time_ranges = move(intersection);
    return result;
}

// The sources' combined ranges follow the steps of HTMLMediaElement's buffered attribute as extended by MSE.
// https://w3c.github.io/media-source/#htmlmediaelement-extensions-buffered
BufferedRanges DemuxerScanState::buffered_ranges_of_sources(ReadonlySpan<CombinedScanState> sources)
{
    if (sources.is_empty())
        return {};

    // A file's declared duration may lie beyond its tracks' data, and counts as buffered once its data has ended.
    AK::Duration highest_end_time;
    for (auto const& source : sources)
        highest_end_time = max(highest_end_time, source.duration);

    TimeRanges intersection { { AK::Duration::zero(), highest_end_time } };
    bool resource_is_complete = true;
    for (auto const& source : sources) {
        resource_is_complete &= source.resource_is_complete;
        auto source_ranges = source.time_ranges;
        if (source.reached_end_of_stream && !source_ranges.is_empty())
            source_ranges.add_range(source_ranges[source_ranges.size() - 1].start, highest_end_time);
        intersection = intersection.intersection(source_ranges);
    }
    return { move(intersection), resource_is_complete };
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::DemuxerScanState const& state)
{
    TRY(encoder.encode(state.track_buffered_ranges));
    TRY(encoder.encode(state.buffered_ranges_policy));
    TRY(encoder.encode(state.reached_end_of_stream));
    TRY(encoder.encode(state.resource_is_complete));
    TRY(encoder.encode(state.duration));
    return {};
}

template<>
ErrorOr<Media::DemuxerScanState> decode(Decoder& decoder)
{
    Media::DemuxerScanState state;
    state.track_buffered_ranges = TRY(decoder.decode<Vector<Media::TrackBufferedRanges>>());
    state.buffered_ranges_policy = TRY(decoder.decode<Media::BufferedRangesPolicy>());
    if (state.buffered_ranges_policy > Media::BufferedRangesPolicy::AllTracks)
        return Error::from_string_literal("IPC: Invalid buffered ranges policy");
    state.reached_end_of_stream = TRY(decoder.decode<bool>());
    state.resource_is_complete = TRY(decoder.decode<bool>());
    state.duration = TRY(decoder.decode<AK::Duration>());
    return state;
}

}
