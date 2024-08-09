/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ALooperEventLoopImplementation.h"
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadEventQueue.h>
#include <jni.h>

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_TimerExecutorService_00024Timer_nativeRun(JNIEnv*, jobject /* thiz */, jlong, jlong);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_TimerExecutorService_00024Timer_nativeRun(JNIEnv*, jobject /* thiz */, jlong native_data, jlong id)
{
    static Core::EventLoop s_event_loop; // Here to exist for this thread

    auto& event_loop_impl = *reinterpret_cast<Ladybird::ALooperEventLoopImplementation*>(native_data);
    auto& thread_data = event_loop_impl.thread_data();

    if (auto timer_data = thread_data.timers.get(id); timer_data.has_value()) {
        auto receiver = timer_data->receiver.strong_ref();
        if (!receiver)
            return;

        if (timer_data->visibility == Core::TimerShouldFireWhenNotVisible::No)
            if (!receiver->is_visible_for_timer_purposes())
                return;

        event_loop_impl.post_event(*receiver, make<Core::TimerEvent>());
    }
    // Flush the event loop on this thread to keep any garbage from building up
    if (auto num_events = s_event_loop.pump(Core::EventLoop::WaitMode::PollForEvents); num_events != 0) {
        dbgln("BUG: Processed {} events on Timer thread!", num_events);
    }
}
