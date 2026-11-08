/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <WebContent/Worker/ConnectionFromClient.h>
#include <WebContent/Worker/DedicatedWorkerThreadLink.h>

namespace WebWorker {

NonnullRefPtr<DedicatedWorkerThreadLink> DedicatedWorkerThreadLink::create(Web::HTML::WorkerAgentOwnerToken owner_token)
{
    return adopt_ref(*new DedicatedWorkerThreadLink(owner_token));
}

DedicatedWorkerThreadLink::DedicatedWorkerThreadLink(Web::HTML::WorkerAgentOwnerToken owner_token)
    : m_owner_token(owner_token)
    , m_owner_event_loop(Core::EventLoop::current_weak())
{
}

DedicatedWorkerThreadLink::~DedicatedWorkerThreadLink() = default;

void DedicatedWorkerThreadLink::attach_worker_connection(ConnectionFromClient& connection)
{
    m_worker_connection = &connection;

    MutexLocker locker(m_mutex);
    m_worker_event_loop = Core::EventLoop::current_weak();
    for (auto& task : m_tasks_posted_before_worker_attached)
        Core::deferred_invoke(run_on_attached_worker_connection(move(task)));
    m_tasks_posted_before_worker_attached.clear();
}

void DedicatedWorkerThreadLink::detach_worker_connection()
{
    m_worker_connection = nullptr;
}

Function<void()> DedicatedWorkerThreadLink::run_on_attached_worker_connection(WorkerConnectionTask task)
{
    return [link = NonnullRefPtr { *this }, task = move(task)] {
        if (link->m_worker_connection)
            task(*link->m_worker_connection);
    };
}

void DedicatedWorkerThreadLink::post_to_worker(WorkerConnectionTask task)
{
    MutexLocker locker(m_mutex);
    if (!m_worker_event_loop) {
        m_tasks_posted_before_worker_attached.append(move(task));
        return;
    }
    if (auto event_loop = m_worker_event_loop->take(); event_loop.is_alive())
        event_loop->deferred_invoke(run_on_attached_worker_connection(move(task)));
}

void DedicatedWorkerThreadLink::post_to_owner(Function<void()> function)
{
    if (auto event_loop = m_owner_event_loop->take(); event_loop.is_alive())
        event_loop->deferred_invoke(move(function));
}

void DedicatedWorkerThreadLink::rtc_transform_init(u64 transform_id, Web::HTML::SerializedTransferRecord options_record)
{
    post_to_worker([transform_id, options_record = move(options_record)](ConnectionFromClient& connection) mutable {
        connection.rtc_transform_init(transform_id, move(options_record));
    });
}

void DedicatedWorkerThreadLink::rtc_transform_teardown(u64 transform_id)
{
    post_to_worker([transform_id](ConnectionFromClient& connection) {
        connection.rtc_transform_teardown(transform_id);
    });
}

void DedicatedWorkerThreadLink::rtc_transform_encoded_audio_frame(u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number)
{
    post_to_worker([transform_id, payload = move(payload), ssrc, payload_type, rtp_timestamp, sequence_number](ConnectionFromClient& connection) mutable {
        connection.rtc_transform_encoded_audio_frame(transform_id, move(payload), ssrc, payload_type, rtp_timestamp, sequence_number);
    });
}

}
