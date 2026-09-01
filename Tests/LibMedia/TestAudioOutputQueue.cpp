/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/Time.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/Audio/SampleSpecification.h>
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
    for (int i = 0; i < 3; ++i)
        EXPECT(queue->read_interleaved(buffer).is_empty());

    producer->append_block(0, 1024);
    producer->wake();
    EXPECT(wait_until([&] { return queued_block_count.load() > 0; }));

    auto samples = queue->read_interleaved(buffer);
    EXPECT_EQ(samples.size(), QUANTUM_SAMPLE_COUNT);
    EXPECT_EQ(samples[0], 0.0f);
    EXPECT_EQ(samples[2], 1.0f);
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
    EXPECT(queue->read_interleaved(buffer).is_empty());

    producer->append_block(0, 1024);
    producer->wake();
    EXPECT(wait_until([&] { return queued_block_count.load() == 2; }));

    // Frame 1 of a stereo block sits at sample 2; the discarded mono block would have put frame 2 there.
    auto samples = queue->read_interleaved(buffer);
    EXPECT_EQ(samples.size(), QUANTUM_SAMPLE_COUNT);
    EXPECT_EQ(samples[2], 1.0f);
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
