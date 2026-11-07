/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibGC/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DedicatedWorkerThreadLink.h>
#include <LibWebCommon/HTML/WorkerAgentForward.h>
#include <WebContent/Worker/Forward.h>

namespace WebWorker {

class DedicatedWorkerThreadLink final : public Web::HTML::DedicatedWorkerThreadLink {
public:
    static NonnullRefPtr<DedicatedWorkerThreadLink> create(Web::HTML::WorkerAgentOwnerToken);
    virtual ~DedicatedWorkerThreadLink() override;

    Web::HTML::WorkerAgentOwnerToken owner_token() const { return m_owner_token; }

    void attach_worker_connection(ConnectionFromClient&);
    void detach_worker_connection();

    void post_to_owner(Function<void()>);

    void report_script_load_failure_to_owner();
    void report_exception_to_owner(Utf16String message, Utf16String filename, u32 lineno, u32 colno);
    void report_content_security_policy_violation_to_owner(u64 policy_index, String effective_directive, URL::URL const& resource);
    void report_close_to_owner();
    void report_death_to_owner();

    virtual void terminate() override;

    virtual void rtc_transform_init(u64 transform_id, Web::HTML::SerializedTransferRecord options_record) override;
    virtual void rtc_transform_teardown(u64 transform_id) override;
    virtual void rtc_transform_encoded_audio_frame(u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number) override;

private:
    explicit DedicatedWorkerThreadLink(Web::HTML::WorkerAgentOwnerToken);

    using WorkerConnectionTask = Function<void(ConnectionFromClient&)>;
    void post_to_worker(WorkerConnectionTask);
    Function<void()> run_on_attached_worker_connection(WorkerConnectionTask);

    Web::HTML::WorkerAgentOwnerToken m_owner_token { 0 };
    NonnullRefPtr<Core::WeakEventLoopReference> m_owner_event_loop;

    Mutex m_mutex;
    RefPtr<Core::WeakEventLoopReference> m_worker_event_loop;
    RefPtr<Web::HTML::ScriptAbortHandle> m_worker_script_abort_handle;
    Vector<WorkerConnectionTask> m_tasks_posted_before_worker_attached;

    ConnectionFromClient* m_worker_connection { nullptr };
};

}

// NB: The tasks the link keeps cross between threads, so they can't be GC::Functions. They capture no cells.
GC_UNTRACED_CAPTURE_WITH_ROOTED_CALLBACKS(WebWorker::DedicatedWorkerThreadLink);
