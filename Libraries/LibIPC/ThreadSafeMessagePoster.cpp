/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Message.h>
#include <LibIPC/ThreadSafeMessagePoster.h>

namespace IPC {

bool ThreadSafeMessagePoster::try_post(MessageBuffer& buffer)
{
    MutexLocker locker(m_mutex);
    if (!m_transport || !m_transport->is_open())
        return false;
    return !buffer.transfer_message(*m_transport).is_error();
}

void ThreadSafeMessagePoster::detach_from_transport()
{
    MutexLocker locker(m_mutex);
    m_transport = nullptr;
}

static bool try_post_bytes_and_raw_attachments(void* poster, ReadonlyBytes bytes, ReadonlySpan<RawAttachment> attachments)
{
    auto buffer = adopt_encoded_message(bytes, attachments);
    return static_cast<ThreadSafeMessagePoster*>(poster)->try_post(buffer);
}

FfiThreadSafeMessagePoster ThreadSafeMessagePoster::to_ffi_with_one_reference()
{
    ref();
    return {
        .poster_with_one_reference = this,
        .try_post = try_post_bytes_and_raw_attachments,
        .ref = [](void* poster) { static_cast<ThreadSafeMessagePoster*>(poster)->ref(); },
        .unref = [](void* poster) { static_cast<ThreadSafeMessagePoster*>(poster)->unref(); },
    };
}

}
