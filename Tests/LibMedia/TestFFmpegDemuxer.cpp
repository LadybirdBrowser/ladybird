/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/Hex.h>
#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

#include "TestMediaCommon.h"

TEST_CASE(accepts_adts_aac)
{
    // Nine ADTS AAC-LC frames containing 48 kHz stereo silence.
    // clang-format off
    static constexpr auto raw_aac_data = to_array<u8>({
        0xff, 0xf1, 0x4c, 0x80, 0x03, 0xdf, 0xfc, 0xde, 0x02, 0x00, 0x4c, 0x61,
        0x76, 0x63, 0x36, 0x31, 0x2e, 0x31, 0x39, 0x2e, 0x31, 0x30, 0x31, 0x00,
        0x42, 0x20, 0x08, 0xc1, 0x18, 0x38, 0xff, 0xf1, 0x4c, 0x80, 0x01, 0xbf,
        0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0xff, 0xf1, 0x4c, 0x80, 0x01,
        0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0xff, 0xf1, 0x4c, 0x80,
        0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0xff, 0xf1, 0x4c,
        0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0xff, 0xf1,
        0x4c, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0xff,
        0xf1, 0x4c, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c,
        0xff, 0xf1, 0x4c, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60, 0x8c,
        0x1c, 0xff, 0xf1, 0x4c, 0x80, 0x01, 0xbf, 0xfc, 0x21, 0x10, 0x04, 0x60,
        0x8c, 0x1c,
    });
    // clang-format on

    auto stream = Media::IncrementallyPopulatedStream::create_from_data(raw_aac_data);
    EXPECT(!Media::FFmpeg::FFmpegDemuxer::from_stream(stream).is_error());
}

TEST_CASE(ignores_attached_pictures)
{
    auto& loop = never_destroyed_event_loop();
    auto mp3_data = MUST(decode_hex(
        "4944330400000000001c4150494300000012000000696d6167652f6a706567000300ffd8ffd9"
        "ffe318c40000000348000000004c414d45342e305555555555555555555555555555555555555555555555555555555555555555554c414d45342e30555555555555555555555555ffe318c43b00000348000000005555555555555555555555555555555555555555555555555555555555555555555555"
        "55555555554c414d45342e30555555555555555555555555ffe318c47600000348000000005555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555ffe318c4b100000348000000005555555555555555555555"
        "555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555ffe318c4c400000348000000005555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555"
        "ffe318c4c400000348000000005555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555"sv));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(mp3_data);
    auto demuxer = MUST(Media::FFmpeg::FFmpegDemuxer::from_stream(stream));

    auto audio_tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    EXPECT_EQ(audio_tracks.size(), 1u);
    MUST(demuxer->create_context_for_track(audio_tracks[0]));
    EXPECT(!demuxer->get_next_sample_for_track(audio_tracks[0]).is_error());

    auto video_tracks = MUST(demuxer->get_tracks_for_type(Media::TrackType::Video));
    EXPECT(video_tracks.is_empty());

    // The picture must not cost the container its navigator, which is what scans the audio track.
    demuxer->set_scan_state_change_handler([] { });
    ScopeGuard remove_handler = [&] { demuxer->set_scan_state_change_handler(nullptr); };
    bool deadline_expired = false;
    auto deadline_timer = Core::Timer::create_single_shot(1'000, [&] { deadline_expired = true; });
    deadline_timer->start();
    loop.spin_until([&] { return demuxer->scan_state().state_for_track(audio_tracks[0]) != nullptr || deadline_expired; });
    EXPECT(!deadline_expired);
}

TEST_CASE(rejects_formats_handled_by_other_demuxers)
{
    auto file = MUST(Core::File::open("./vfr.mkv"sv, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));

    EXPECT(Media::FFmpeg::FFmpegDemuxer::from_stream(stream).is_error());
}

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
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_blocked { false };

    demuxer->set_read_blocked_change_handler_for_track(track, [&](Media::ReadBlocked blocked) {
        read_blocked = blocked == Media::ReadBlocked::Yes;
    });

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
    while (!read_blocked.load()) { }

    // Abort the blocked read from the main thread.
    demuxer->set_blocking_reads_aborted_for_track(track);

    // Wait for the reader thread to observe the abort.
    while (!got_aborted.load()) { }

    // Reset the abort state and provide the rest of the file data.
    demuxer->reset_blocking_reads_aborted_for_track(track);
    stream->add_chunk_at(initial_chunk_size, file_data.bytes().slice(initial_chunk_size));
    stream->close();

    // Wait for the reader thread to finish. It should successfully read all remaining frames
    // and then get EndOfStream.
    MUST(reader_thread->join());
}
