/*
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/WebAudio/ControlMessage.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>

TEST_CASE(drain_returns_all_and_clears)
{
    Web::WebAudio::ControlMessageQueue queue;

    queue.enqueue(Web::WebAudio::StartSource { .node_id = Web::WebAudio::NodeID { 0 }, .when = 1.0 });
    queue.enqueue(Web::WebAudio::StopSource { .node_id = Web::WebAudio::NodeID { 1 }, .when = 2.0 });

    auto batch = queue.drain();
    EXPECT_EQ(batch.size(), 2u);

    auto empty = queue.drain();
    EXPECT_EQ(empty.size(), 0u);
}

TEST_CASE(drain_preserves_first_in_first_out)
{
    Web::WebAudio::ControlMessageQueue queue;

    queue.enqueue(Web::WebAudio::StartSource { .node_id = Web::WebAudio::NodeID { 0 }, .when = 1.0 });
    queue.enqueue(Web::WebAudio::StopSource { .node_id = Web::WebAudio::NodeID { 1 }, .when = 2.0 });
    queue.enqueue(Web::WebAudio::StartSource { .node_id = Web::WebAudio::NodeID { 2 }, .when = 3.0 });

    auto batch = queue.drain();
    EXPECT_EQ(batch.size(), 3u);

    EXPECT(batch[0].has<Web::WebAudio::StartSource>());
    EXPECT_EQ(batch[0].get<Web::WebAudio::StartSource>().when, 1.0);
    EXPECT_EQ(batch[0].get<Web::WebAudio::StartSource>().node_id, 0u);

    EXPECT(batch[1].has<Web::WebAudio::StopSource>());
    EXPECT_EQ(batch[1].get<Web::WebAudio::StopSource>().when, 2.0);
    EXPECT_EQ(batch[1].get<Web::WebAudio::StopSource>().node_id, Web::WebAudio::NodeID { 1 });

    EXPECT(batch[2].has<Web::WebAudio::StartSource>());
    EXPECT_EQ(batch[2].get<Web::WebAudio::StartSource>().when, 3.0);
    EXPECT_EQ(batch[2].get<Web::WebAudio::StartSource>().node_id, Web::WebAudio::NodeID { 2 });
}
