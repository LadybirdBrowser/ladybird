/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Sinks/AudioOutputQueue.h>
#include <LibTest/TestCase.h>
#include <unistd.h>

#include "TestMediaCommon.h"

static constexpr size_t QUANTUM_SAMPLE_COUNT = 256;

template<typename Condition>
static bool wait_until(Condition condition)
{
    auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(5);
    while (!condition()) {
        if (MonotonicTime::now_coarse() >= deadline)
            return false;
        usleep(1'000);
    }
    return true;
}

template<typename Condition>
static bool pump_until(Core::EventLoop& loop, Condition condition)
{
    auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(5);
    while (!condition()) {
        if (MonotonicTime::now_coarse() >= deadline)
            return false;
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    return true;
}

TEST_CASE(reads_hold_their_position_while_the_queue_was_empty)
{
    never_destroyed_event_loop();
    auto producer = ScriptedAudioProducer::create();
    auto queue = TRY_OR_FAIL(Media::AudioOutputQueue::try_create(nullptr));
    Atomic<size_t> queued_block_count { 0 };
    queue->set_data_available_handler([&] { queued_block_count.fetch_add(1); });
    TRY_OR_FAIL(queue->set_sample_specification({ 48'000, Audio::ChannelMap::stereo() }));
    TRY_OR_FAIL(queue->connect_input(producer));
    queue->start();
    queue->seek(AK::Duration::zero());

    Array<float, QUANTUM_SAMPLE_COUNT> buffer;
    for (int i = 0; i < 3; ++i) {
        auto empty_read = queue->read_interleaved(buffer, {});
        EXPECT(empty_read.samples.is_empty());
        EXPECT(!empty_read.contains_non_silent_samples);
    }

    producer->append_block(0, 1024);
    producer->wake();
    EXPECT(wait_until([&] { return queued_block_count.load() > 0; }));

    auto read = queue->read_interleaved(buffer, {});
    EXPECT_EQ(read.samples.size(), QUANTUM_SAMPLE_COUNT);
    EXPECT_EQ(read.samples[0], 0.0f);
    EXPECT_EQ(read.samples[2], 1.0f);
    EXPECT(read.contains_non_silent_samples);
    queue->stop();
}

TEST_CASE(changing_the_sample_specification_discards_queued_blocks)
{
    never_destroyed_event_loop();
    auto producer = ScriptedAudioProducer::create();
    auto queue = TRY_OR_FAIL(Media::AudioOutputQueue::try_create(nullptr));
    Atomic<size_t> queued_block_count { 0 };
    queue->set_data_available_handler([&] { queued_block_count.fetch_add(1); });
    TRY_OR_FAIL(queue->set_sample_specification({ 48'000, Audio::ChannelMap::mono() }));
    TRY_OR_FAIL(queue->connect_input(producer));
    queue->start();
    queue->seek(AK::Duration::zero());

    producer->append_block(0, 1024);
    producer->wake();
    EXPECT(wait_until([&] { return queued_block_count.load() == 1; }));

    TRY_OR_FAIL(queue->set_sample_specification({ 48'000, Audio::ChannelMap::stereo() }));

    Array<float, QUANTUM_SAMPLE_COUNT> buffer;
    auto stale_read = queue->read_interleaved(buffer, {});
    EXPECT(stale_read.samples.is_empty());
    EXPECT_EQ(stale_read.channel_count, 2u);

    producer->append_block(0, 1024);
    producer->wake();
    EXPECT(wait_until([&] { return queued_block_count.load() == 2; }));

    // Frame 1 of a stereo block sits at sample 2; the discarded mono block would have put frame 2 there.
    auto read = queue->read_interleaved(buffer, {});
    EXPECT_EQ(read.samples.size(), QUANTUM_SAMPLE_COUNT);
    EXPECT_EQ(read.channel_count, 2u);
    EXPECT_EQ(read.samples[2], 1.0f);
    queue->stop();
}

TEST_CASE(does_not_pull_before_being_started)
{
    never_destroyed_event_loop();
    auto producer = ScriptedAudioProducer::create();
    auto queue = TRY_OR_FAIL(Media::AudioOutputQueue::try_create(nullptr));
    Atomic<size_t> queued_block_count { 0 };
    queue->set_data_available_handler([&] { queued_block_count.fetch_add(1); });
    TRY_OR_FAIL(queue->set_sample_specification({ 48'000, Audio::ChannelMap::stereo() }));
    TRY_OR_FAIL(queue->connect_input(producer));
    queue->seek(AK::Duration::zero());
    producer->append_block(0, 1024);
    producer->wake();

    usleep(50'000);
    EXPECT_EQ(producer->peek_count(), 0u);

    queue->start();
    EXPECT(wait_until([&] { return queued_block_count.load() > 0; }));
    queue->stop();
}

TEST_CASE(end_of_stream_is_reported_once_the_queued_audio_has_been_read)
{
    auto& loop = never_destroyed_event_loop();
    auto producer = ScriptedAudioProducer::create();
    auto queue = TRY_OR_FAIL(Media::AudioOutputQueue::try_create(nullptr));
    Vector<Media::PipelineStatus> statuses;
    queue->set_state_change_handler([&](Media::PipelineStatus status) { statuses.append(status); });
    Atomic<size_t> queued_block_count { 0 };
    queue->set_data_available_handler([&] { queued_block_count.fetch_add(1); });
    TRY_OR_FAIL(queue->set_sample_specification({ 48'000, Audio::ChannelMap::stereo() }));
    TRY_OR_FAIL(queue->connect_input(producer));
    queue->start();
    queue->seek(AK::Duration::zero());

    producer->append_block(0, 256);
    producer->append_status(Media::PipelineStatus::EndOfStream);
    producer->wake();

    // The block, then the silence the queue keeps appending behind the end of the stream.
    EXPECT(wait_until([&] { return queued_block_count.load() >= 2; }));
    EXPECT(pump_until(loop, [&] { return !statuses.is_empty(); }));
    EXPECT_EQ(statuses, (Vector { Media::PipelineStatus::HaveData }));

    Array<float, QUANTUM_SAMPLE_COUNT> buffer;
    EXPECT_EQ(queue->read_interleaved(buffer, {}).samples.size(), QUANTUM_SAMPLE_COUNT);
    EXPECT_EQ(queue->read_interleaved(buffer, {}).samples.size(), QUANTUM_SAMPLE_COUNT);

    EXPECT(pump_until(loop, [&] { return statuses.size() == 2; }));
    EXPECT_EQ(statuses, (Vector { Media::PipelineStatus::HaveData, Media::PipelineStatus::EndOfStream }));
    queue->stop();
}
