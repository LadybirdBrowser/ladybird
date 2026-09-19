/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    if (size > 1024 * 1024)
        return 0;
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    if (size > 0)
        stream->add_chunk_at(0, { data, size });
    stream->close();
    auto reader = Media::Matroska::Reader::from_stream(stream->create_cursor());
    if (reader.is_error())
        return 0;
    // The factory starts a detached scan thread with no join barrier. Use the
    // production demuxer constructor so iterations cannot leak background work.
    auto demuxer = make_ref_counted<Media::Matroska::MatroskaDemuxer>(stream, reader.release_value());
    for (auto type : { Media::TrackType::Video, Media::TrackType::Audio }) {
        auto preferred = demuxer->get_preferred_track_for_type(type);
        if (preferred.is_error() || !preferred.value().has_value())
            continue;
        auto track = preferred.value().value();
        if (demuxer->create_context_for_track(track).is_error())
            continue;
        (void)demuxer->duration_of_track(track);
        for (size_t i = 0; i < 16; ++i) {
            if (demuxer->get_next_sample_for_track(track).is_error())
                break;
        }
        for (i64 milliseconds : { 0, 1, 1000, 60000 }) {
            auto seek = demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_milliseconds(milliseconds), Media::DemuxerSeekOptions::Force);
            if (!seek.is_error())
                (void)demuxer->get_next_sample_for_track(track);
        }
        demuxer->set_blocking_reads_aborted_for_track(track);
        (void)demuxer->get_next_sample_for_track(track);
        demuxer->reset_blocking_reads_aborted_for_track(track);
        (void)demuxer->get_next_sample_for_track(track);
    }
    return 0;
}
