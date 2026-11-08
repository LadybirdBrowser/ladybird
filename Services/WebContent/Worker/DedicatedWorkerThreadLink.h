/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
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
    Vector<WorkerConnectionTask> m_tasks_posted_before_worker_attached;

    ConnectionFromClient* m_worker_connection { nullptr };
};

}
