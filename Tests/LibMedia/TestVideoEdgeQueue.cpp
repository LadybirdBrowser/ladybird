/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibCore/System.h>
#include <LibMedia/VideoEdgeQueue.h>
#include <LibTest/TestCase.h>

using namespace Media;

TEST_CASE(enqueue_peek_consume)
{
    auto edge = MUST(VideoEdgeQueue::create());
    EXPECT(!edge.peek().has_value());

    VideoFrameHandle handle;
    handle.timestamp = AK::Duration::from_milliseconds(100);
    MUST(edge.enqueue(handle, 1));

    auto item = edge.peek();
    EXPECT(item.has_value());
    EXPECT_EQ(item->seek_id, 1u);
    EXPECT_EQ(item->handle.timestamp, AK::Duration::from_milliseconds(100));
    // peek() does not advance.
    EXPECT(edge.peek().has_value());

    edge.consume();
    EXPECT(!edge.peek().has_value());
}

TEST_CASE(header_carries_status)
{
    auto edge = MUST(VideoEdgeQueue::create());
    auto initial = edge.status();
    EXPECT(initial.has_value());
    EXPECT_EQ(initial->status, PipelineStatus::Pending);

    edge.set_status(PipelineStatus::EndOfStream, 7);
    auto updated = edge.status();
    EXPECT(updated.has_value());
    EXPECT_EQ(updated->status, PipelineStatus::EndOfStream);
    EXPECT_EQ(updated->seek_id, 7u);
}

TEST_CASE(consumer_maps_the_same_edge_through_fds)
{
    auto producer = MUST(VideoEdgeQueue::create());
    // Cross-process, IPC hands the consumer a dup'd fd; emulate that here so producer and consumer
    // don't both close the same one on teardown.
    auto ring_fd = MUST(Core::System::dup(producer.ring_fd()));
    auto consumer = MUST(VideoEdgeQueue::create(ring_fd, producer.header_buffer()));

    VideoFrameHandle handle;
    handle.slot_index = 3;
    MUST(producer.enqueue(handle, 2));
    producer.set_status(PipelineStatus::HaveData, 2);

    auto item = consumer.peek();
    EXPECT(item.has_value());
    EXPECT_EQ(item->handle.slot_index, 3u);
    EXPECT_EQ(item->seek_id, 2u);
    auto status = consumer.status();
    EXPECT(status.has_value());
    EXPECT_EQ(status->status, PipelineStatus::HaveData);

    consumer.consume();
    EXPECT(!consumer.peek().has_value());
}
