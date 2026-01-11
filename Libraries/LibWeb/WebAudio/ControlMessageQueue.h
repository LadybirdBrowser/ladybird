/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/ControlMessage.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#control-message-queue
class WEB_API ControlMessageQueue {

public:
    void enqueue(ControlMessage);   // Called by the control thread.
    Vector<ControlMessage> drain(); // Called by the rendering thread.

private:
    mutable Threading::Mutex m_mutex;
    Vector<ControlMessage> m_messages;
};

}
