/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibThreading/Mutex.h>
#include <WebAudioWorker/MainEventLoop.h>

namespace Web::WebAudio {

static Threading::Mutex s_main_event_loop_mutex;
static RefPtr<Core::WeakEventLoopReference> s_main_event_loop_reference;

void set_main_event_loop_reference(NonnullRefPtr<Core::WeakEventLoopReference> reference)
{
    Threading::MutexLocker locker(s_main_event_loop_mutex);
    s_main_event_loop_reference = move(reference);
}

RefPtr<Core::WeakEventLoopReference> main_event_loop()
{
    Threading::MutexLocker locker(s_main_event_loop_mutex);
    return s_main_event_loop_reference;
}

}
