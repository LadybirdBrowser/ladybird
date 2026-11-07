/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Mutex.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/ControlMessage.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#control-message-queue
class WEB_API ControlMessageQueue : public AtomicRefCounted<ControlMessageQueue> {
public:
    ControlMessageQueue();
    ~ControlMessageQueue();
    void enqueue(ControlMessage); // Called by the control thread.
    void const* rust_queue() const { return m_queue; }
    void* drain(); // Called by the rendering thread.

private:
    void const* m_queue { nullptr };
};

}
