/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/ControlMessageQueue.h>
namespace Web::WebAudio {

void ControlMessageQueue::enqueue(ControlMessage message)
{
    MutexLocker locker(m_mutex);
    m_messages.append(move(message));
}

Vector<ControlMessage> ControlMessageQueue::drain()
{
    MutexLocker locker(m_mutex);
    return move(m_messages);
}

}
