/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibMedia/DecoderRegistrations.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/Sinks/AudioOutputFactory.h>
#include <LibTest/TestCase.h>

#include "TestMediaCommon.h"

namespace {

TEST_SETUP
{
    Media::install_audio_decoders({ Media::audio_decoders() });
}

ByteBuffer read_fixture(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return MUST(file->read_until_eof());
}

bool spin_until(Core::EventLoop& loop, Function<bool()> condition, AK::Duration timeout)
{
    auto deadline_expired = false;
    auto deadline_timer = Core::Timer::create_single_shot(timeout, [&] { deadline_expired = true; });
    deadline_timer->start();
    loop.spin_until([&] { return condition() || deadline_expired; });
    return !deadline_expired;
}

// Playing out at the download head pauses the output, whose discard reseeks the input and thereby aborts the blocked
// read. That must not pass for the data arriving, or playback resumes into the same block and loops.
TEST_CASE(buffering_at_the_download_head_holds_until_data_arrives)
{
    auto& loop = never_destroyed_event_loop();
    auto file_data = read_fixture("bbb_without_stream_info.mp3"sv);

    // At 128 kb/s, about the first second of audio arrives, then nothing more.
    constexpr size_t served_size = 16'000;
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(file_data.size());
    stream->add_chunk_at(0, file_data.bytes().trim(served_size));
    stream->set_data_request_callback([](Optional<u64>) { });

    auto manager = Media::PlaybackManager::create(Media::AudioOutputFactory::create_null());
    Vector<Media::PlaybackState> states;
    manager->on_playback_state_change = [&] { states.append(manager->state()); };

    bool source_added = false;
    manager->add_media_source(stream)->when_resolved([&](auto&) { source_added = true; });
    EXPECT(spin_until(loop, [&] { return source_added; }, AK::Duration::from_seconds(2)));
    if (manager->audio_tracks().is_empty()) {
        FAIL("Expected an audio track");
        return;
    }
    manager->enable_an_audio_track(manager->audio_tracks()[0], Media::PlaybackManager::ResumeEndedPlayback::No);
    manager->start();
    manager->play();

    EXPECT(spin_until(loop, [&] { return states.contains_slow(Media::PlaybackState::Playing); }, AK::Duration::from_seconds(2)));
    EXPECT(spin_until(loop, [&] { return manager->state() == Media::PlaybackState::Buffering; }, AK::Duration::from_seconds(3)));

    auto transitions_when_buffering_began = states.size();
    spin_until(loop, [] { return false; }, AK::Duration::from_milliseconds(300));
    EXPECT_EQ(states.size(), transitions_when_buffering_began);
    EXPECT_EQ(manager->state(), Media::PlaybackState::Buffering);

    stream->add_chunk_at(served_size, file_data.bytes().slice(served_size));
    stream->close();
    EXPECT(spin_until(loop, [&] { return manager->state() == Media::PlaybackState::Playing; }, AK::Duration::from_seconds(2)));

    manager->on_playback_state_change = nullptr;
    stream->set_data_request_callback(nullptr);
}

OwnPtr<Media::PlaybackManager> play_to_end(Core::EventLoop& loop)
{
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(read_fixture("Ogg/sine_opus_1005ms.ogg"sv));
    auto manager = Media::PlaybackManager::create(Media::AudioOutputFactory::create_null());

    bool source_added = false;
    manager->add_media_source(stream)->when_resolved([&](auto&) { source_added = true; });
    if (!spin_until(loop, [&] { return source_added; }, AK::Duration::from_seconds(2)) || manager->audio_tracks().is_empty())
        return {};
    manager->enable_an_audio_track(manager->audio_tracks()[0], Media::PlaybackManager::ResumeEndedPlayback::No);
    manager->start();
    manager->play();
    if (!spin_until(loop, [&] { return manager->state() == Media::PlaybackState::Playing; }, AK::Duration::from_seconds(2)))
        return {};

    manager->seek(manager->duration() - AK::Duration::from_milliseconds(100), Media::SeekMode::Accurate);
    if (!spin_until(loop, [&] { return manager->state() == Media::PlaybackState::Ended; }, AK::Duration::from_seconds(2)))
        return {};
    return manager;
}

// A media element that loops seeks back to the start once playback ends, and expects playback to continue.
TEST_CASE(seek_after_playing_to_the_end_continues_playing)
{
    auto& loop = never_destroyed_event_loop();
    auto manager = play_to_end(loop);
    if (!manager) {
        FAIL("Expected playback to end");
        return;
    }

    manager->seek(AK::Duration::zero(), Media::SeekMode::Accurate);
    EXPECT(spin_until(loop, [&] { return manager->state() != Media::PlaybackState::Seeking; }, AK::Duration::from_seconds(2)));
    EXPECT_EQ(manager->state(), Media::PlaybackState::Playing);
}

TEST_CASE(seek_after_pausing_at_the_end_stays_paused)
{
    auto& loop = never_destroyed_event_loop();
    auto manager = play_to_end(loop);
    if (!manager) {
        FAIL("Expected playback to end");
        return;
    }

    manager->pause();
    manager->seek(AK::Duration::zero(), Media::SeekMode::Accurate);
    EXPECT(spin_until(loop, [&] { return manager->state() != Media::PlaybackState::Seeking; }, AK::Duration::from_seconds(2)));
    EXPECT_EQ(manager->state(), Media::PlaybackState::Paused);
}

}
