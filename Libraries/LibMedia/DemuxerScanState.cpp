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
        state.track_buffered_ranges.append(move(scan.time_ranges));
    }
    return state;
}

AK::Duration DemuxerScanState::highest_track_end_time() const
{
    AK::Duration highest_end_time;
    for (auto const& track_ranges : track_buffered_ranges)
        highest_end_time = max(highest_end_time, track_ranges.highest_end_time());
    return highest_end_time;
}

// A source's ranges follow the steps of SourceBuffer's buffered attribute.
// https://w3c.github.io/media-source/#dom-sourcebuffer-buffered
TimeRanges DemuxerScanState::buffered_ranges() const
{
    auto highest_end_time = highest_track_end_time();

    TimeRanges intersection { { AK::Duration::zero(), highest_end_time } };
    for (auto const& ranges : track_buffered_ranges) {
        auto track_ranges = ranges;
        if (reached_end_of_stream && !track_ranges.is_empty())
            track_ranges.add_range(track_ranges[track_ranges.size() - 1].start, highest_end_time);
        intersection = intersection.intersection(track_ranges);
    }
    return intersection;
}

// The sources' combined ranges follow the steps of HTMLMediaElement's buffered attribute as extended by MSE.
// https://w3c.github.io/media-source/#htmlmediaelement-extensions-buffered
TimeRanges DemuxerScanState::buffered_ranges_of_sources(ReadonlySpan<DemuxerScanState> sources)
{
    if (sources.is_empty())
        return {};

    // A file's declared duration may lie beyond its tracks' data, and counts as buffered once its data has ended.
    AK::Duration highest_end_time;
    for (auto const& source : sources)
        highest_end_time = max(highest_end_time, source.duration);

    TimeRanges intersection { { AK::Duration::zero(), highest_end_time } };
    for (auto const& source : sources) {
        auto source_ranges = source.buffered_ranges();
        if (source.reached_end_of_stream && !source_ranges.is_empty())
            source_ranges.add_range(source_ranges[source_ranges.size() - 1].start, highest_end_time);
        intersection = intersection.intersection(source_ranges);
    }
    return intersection;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::DemuxerScanState const& state)
{
    TRY(encoder.encode(state.track_buffered_ranges));
    TRY(encoder.encode(state.reached_end_of_stream));
    TRY(encoder.encode(state.duration));
    return {};
}

template<>
ErrorOr<Media::DemuxerScanState> decode(Decoder& decoder)
{
    Media::DemuxerScanState state;
    state.track_buffered_ranges = TRY(decoder.decode<Vector<Media::TimeRanges>>());
    state.reached_end_of_stream = TRY(decoder.decode<bool>());
    state.duration = TRY(decoder.decode<AK::Duration>());
    return state;
}

}
