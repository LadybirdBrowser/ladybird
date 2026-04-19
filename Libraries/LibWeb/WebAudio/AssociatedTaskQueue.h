/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/Vector.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/Export.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#rendering-loop
// The spec defines an "associated task queue" for BaseAudioContext which is processed on the
// rendering thread at the start of each render quantum ("rendering a render quantum", step 3).
//
// NOTE: This queue is currently an internal plumbing point. Tasks must be render-thread-safe and
// must not touch JS/GC-managed objects.
class WEB_API AssociatedTaskQueue {
public:
    using Task = Function<void()>;

    ~AssociatedTaskQueue();

    void set_wake_callback(Function<void()>);
    void enqueue(Task);
    Vector<Task> drain();

private:
    struct Node {
        Task task;
        Node* next { nullptr };
    };

    Atomic<Node*> m_head { nullptr };

    Threading::Mutex m_wake_callback_mutex;
    Function<void()> m_wake_callback;
};

}
