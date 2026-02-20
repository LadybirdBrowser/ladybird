/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibTest/TestSuite.h>

TEST_CASE(video_seek_callback_does_not_retain_removed_display_sink)
{
    Core::EventLoop event_loop;
    NonnullRefPtr<Media::PlaybackManager> playback_manager = Media::PlaybackManager::create();

    bool metadata_parsed = false;
    playback_manager->on_metadata_parsed = [&] { metadata_parsed = true; };

    NonnullOwnPtr<Core::File> file = MUST([&]() -> ErrorOr<NonnullOwnPtr<Core::File>> {
        auto in_local_dir = Core::File::open("vfr.mkv"sv, Core::File::OpenMode::Read);
        if (!in_local_dir.is_error())
            return in_local_dir.release_value();
        return Core::File::open("Tests/LibMedia/vfr.mkv"sv, Core::File::OpenMode::Read);
    }());
    NonnullRefPtr<Media::IncrementallyPopulatedStream> stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    playback_manager->add_media_source(stream);

    MonotonicTime deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(2);
    while (!metadata_parsed && MonotonicTime::now_coarse() < deadline)
        event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    EXPECT(metadata_parsed);
    if (!metadata_parsed)
        return;

    EXPECT(!playback_manager->video_tracks().is_empty());
    if (playback_manager->video_tracks().is_empty())
        return;

    Media::Track track = playback_manager->preferred_video_track().value_or(playback_manager->video_tracks().first());
    NonnullRefPtr<Media::DisplayingVideoSink> display = playback_manager->get_or_create_the_displaying_video_sink_for_track(track);
    playback_manager->remove_the_displaying_video_sink_for_track(track);

    EXPECT_EQ(display->ref_count(), 1u);
}
