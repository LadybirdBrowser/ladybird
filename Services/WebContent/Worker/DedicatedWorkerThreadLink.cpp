/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/HTML/Scripting/ScriptAbortHandle.h>
#include <LibWeb/HTML/WorkerAgentParent.h>
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
    m_worker_script_abort_handle = connection.script_abort_handle();
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

void DedicatedWorkerThreadLink::terminate()
{
    RefPtr<Web::HTML::ScriptAbortHandle> worker_script_abort_handle;
    {
        MutexLocker locker(m_mutex);
        worker_script_abort_handle = m_worker_script_abort_handle;
    }
    if (worker_script_abort_handle)
        worker_script_abort_handle->abort_script();

    post_to_worker([](ConnectionFromClient& connection) {
        connection.close_worker();
    });
}

void DedicatedWorkerThreadLink::report_script_load_failure_to_owner()
{
    post_to_owner([owner_token = m_owner_token] {
        Web::HTML::WorkerAgentParent::did_fail_loading_worker_script(owner_token);
    });
}

void DedicatedWorkerThreadLink::report_exception_to_owner(Utf16String message, Utf16String filename, u32 lineno, u32 colno)
{
    post_to_owner([owner_token = m_owner_token, message = move(message), filename = move(filename), lineno, colno] mutable {
        Web::HTML::WorkerAgentParent::did_report_worker_exception(owner_token, move(message), move(filename), lineno, colno);
    });
}

void DedicatedWorkerThreadLink::report_content_security_policy_violation_to_owner(u64 policy_index, String effective_directive, URL::URL const& resource)
{
    auto resource_without_thread_unsafe_blob_url_entry = resource;
    resource_without_thread_unsafe_blob_url_entry.set_blob_url_entry({});
    post_to_owner([owner_token = m_owner_token, policy_index, effective_directive = move(effective_directive), resource = move(resource_without_thread_unsafe_blob_url_entry)] mutable {
        Web::HTML::WorkerAgentParent::did_report_content_security_policy_violation(owner_token, policy_index, move(effective_directive), move(resource));
    });
}

void DedicatedWorkerThreadLink::report_close_to_owner()
{
    post_to_owner([owner_token = m_owner_token] {
        Web::HTML::WorkerAgentParent::did_close_worker(owner_token);
    });
}

void DedicatedWorkerThreadLink::report_death_to_owner()
{
    post_to_owner([owner_token = m_owner_token] {
        Web::HTML::WorkerAgentParent::did_worker_agent_die(owner_token);
    });
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
