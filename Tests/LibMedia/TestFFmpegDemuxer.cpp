/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <LibCore/File.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

TEST_CASE(read_after_aborted_blocking_read)
{
    // This is a regression test for an issue that would occur when aborting a blocking read in the AVIOContext
    // underlying an  FFmpegDemuxer. We would return AVERROR_EXIT when aborting reads, but libavformat holds onto
    // any non-EOF error and only returns that error upon subsequent EOF reads. This would cause our playback system
    // to encounter an unexpected error when playing to the end of a file after an aborted read.

    // The fix was to only return AVERROR_EOF from the IO context callbacks, and then determine whether to change
    // the error to an Aborted error within the FFmpegDemuxer on top of the avformat context that used the IO.

    auto file = MUST(Core::File::open("./avc.mp4"sv, Core::File::OpenMode::Read));
    auto file_data = MUST(file->read_until_eof());

    // Feed only a portion of the file into the stream so that the demuxer will eventually block
    // waiting for more data.
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    auto initial_chunk_size = file_data.size() / 4;
    stream->set_expected_size(file_data.size());
    stream->add_chunk_at(0, file_data.bytes().trim(initial_chunk_size));

    // Create the demuxer from the partial stream.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto demuxer = MUST(Media::FFmpeg::FFmpegDemuxer::from_stream(stream));
    auto optional_track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video));
    VERIFY(optional_track.has_value());
    IGNORE_USE_IN_ESCAPING_LAMBDA auto track = optional_track.release_value();
    MUST(demuxer->create_context_for_track(track));

    // Start a thread to read the frames in parallel and check the errors returned.
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> got_aborted { false };

    auto reader_thread = Threading::Thread::construct("TestReader"sv, [&]() -> intptr_t {
        // Read frames until a read blocks and is aborted.
        while (true) {
            auto sample_result = demuxer->get_next_sample_for_track(track);
            if (sample_result.is_error()) {
                EXPECT_EQ(sample_result.error().category(), Media::DecoderErrorCategory::Aborted);
                got_aborted = true;
                break;
            }
        }

        // After abort is reset and remaining data is added, read all remaining frames.
        // We must eventually get EndOfStream, not a stale error.
        while (true) {
            auto sample_result = demuxer->get_next_sample_for_track(track);

            // Ignore any spurious aborts. This could be avoided with another atomic bool, but it is going to be
            // a very short spin.
            if (sample_result.is_error() && sample_result.error().category() == Media::DecoderErrorCategory::Aborted)
                continue;

            if (sample_result.is_error()) {
                EXPECT_EQ(sample_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
                break;
            }
        }

        return 0;
    });

    reader_thread->start();

    // Wait for the reader thread to block on a read.
    while (!demuxer->is_read_blocked_for_track(track)) { }

    // Abort the blocked read from the main thread.
    demuxer->set_blocking_reads_aborted_for_track(track);

    // Wait for the reader thread to observe the abort.
    while (!got_aborted.load()) { }

    // Reset the abort state and provide the rest of the file data.
    demuxer->reset_blocking_reads_aborted_for_track(track);
    stream->add_chunk_at(initial_chunk_size, file_data.bytes().slice(initial_chunk_size));
    stream->reached_end_of_body();

    // Wait for the reader thread to finish. It should successfully read all remaining frames
    // and then get EndOfStream.
    MUST(reader_thread->join());
}
