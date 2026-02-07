/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/Vector.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/ControlMessage.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#control-message-queue
// NOTE: The spec describes processing control messages by atomically swapping the current control
// message queue with an empty queue (see §2.5 Rendering an Audio Graph). Our implementation
// instead drains the queue under a mutex and moves out the message vector.
class WEB_API ControlMessageQueue {

public:
    ~ControlMessageQueue();

    void set_wake_callback(Function<void()>);
    void enqueue(ControlMessage);   // Called by the control thread.
    Vector<ControlMessage> drain(); // Called by the rendering thread.

private:
    struct Node {
        ControlMessage message;
        Node* next { nullptr };
    };

    Atomic<Node*> m_head { nullptr };

    Threading::Mutex m_wake_callback_mutex;
    Function<void()> m_wake_callback;
};

}
