/*
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/WebAudio/ControlMessage.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/Debug.h>

#include <pthread.h>

TEST_CASE(drain_returns_all_and_clears)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    Web::WebAudio::ControlMessageQueue queue;

    queue.enqueue(Web::WebAudio::StartSource { .node_id = Web::WebAudio::NodeID { 0 }, .when = 1.0 });
    queue.enqueue(Web::WebAudio::StopSource { .node_id = Web::WebAudio::NodeID { 1 }, .when = 2.0 });

    struct DrainContext {
        Web::WebAudio::ControlMessageQueue* queue;
        Vector<Web::WebAudio::ControlMessage>* batch;
        Vector<Web::WebAudio::ControlMessage>* empty;
    };

    Vector<Web::WebAudio::ControlMessage> batch;
    Vector<Web::WebAudio::ControlMessage> empty;

    DrainContext context { .queue = &queue, .batch = &batch, .empty = &empty };

    pthread_t tid {};
    auto* context_ptr = &context;
    auto thread_entry = [](void* arg) -> void* {
        Web::WebAudio::mark_current_thread_as_render_thread();
        auto& ctx = *static_cast<DrainContext*>(arg);
        *ctx.batch = ctx.queue->drain();
        *ctx.empty = ctx.queue->drain();
        return nullptr;
    };
    int rc = pthread_create(&tid, nullptr, thread_entry, context_ptr);
    EXPECT_EQ(rc, 0);
    if (rc == 0)
        (void)pthread_join(tid, nullptr);

    EXPECT_EQ(batch.size(), 2u);
    EXPECT_EQ(empty.size(), 0u);
}

TEST_CASE(drain_preserves_first_in_first_out)
{
    Web::WebAudio::mark_current_thread_as_control_thread();

    Web::WebAudio::ControlMessageQueue queue;

    queue.enqueue(Web::WebAudio::StartSource { .node_id = Web::WebAudio::NodeID { 0 }, .when = 1.0 });
    queue.enqueue(Web::WebAudio::StopSource { .node_id = Web::WebAudio::NodeID { 1 }, .when = 2.0 });
    queue.enqueue(Web::WebAudio::StartSource { .node_id = Web::WebAudio::NodeID { 2 }, .when = 3.0 });

    struct DrainContext {
        Web::WebAudio::ControlMessageQueue* queue;
        Vector<Web::WebAudio::ControlMessage>* batch;
    };

    Vector<Web::WebAudio::ControlMessage> batch;
    DrainContext context { .queue = &queue, .batch = &batch };

    pthread_t tid {};
    auto* context_ptr = &context;
    auto thread_entry = [](void* arg) -> void* {
        Web::WebAudio::mark_current_thread_as_render_thread();
        auto& ctx = *static_cast<DrainContext*>(arg);
        *ctx.batch = ctx.queue->drain();
        return nullptr;
    };
    int rc = pthread_create(&tid, nullptr, thread_entry, context_ptr);
    EXPECT_EQ(rc, 0);
    if (rc == 0)
        (void)pthread_join(tid, nullptr);

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
