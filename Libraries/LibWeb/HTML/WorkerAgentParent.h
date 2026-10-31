/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Utf16String.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Worker.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HTML/WorkerAgentTypes.h>

namespace Web::HTML {

using WorkerOptions = Bindings::WorkerOptions;

// FIXME: Figure out a better naming convention for this type of parent/child process pattern.
class WorkerAgentParent : public JS::Cell {
    GC_CELL(WorkerAgentParent, JS::Cell);
    GC_DECLARE_ALLOCATOR(WorkerAgentParent);

public:
    static GC::Ref<WorkerAgentParent> create(URL::URL, WorkerOptions const&,
        GC::Ptr<MessagePort> outside_port, GC::Ref<EnvironmentSettingsObject> outside_settings,
        GC::Ref<DOM::EventTarget> worker_event_target, AgentType);

    static WEB_API void did_fail_loading_worker_script(WorkerAgentOwnerToken);
    static WEB_API void did_report_worker_exception(WorkerAgentOwnerToken, Utf16String message, Utf16String filename, u32 lineno, u32 colno);
    static WEB_API void did_report_content_security_policy_violation(WorkerAgentOwnerToken, u64 policy_index, String effective_directive, URL::URL resource);
    static WEB_API void did_close_worker(WorkerAgentOwnerToken);
    static WEB_API void did_worker_agent_die(WorkerAgentOwnerToken);
    static WEB_API void did_write_rtc_transform_encoded_audio_frame(WorkerAgentOwnerToken, u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number);

    void terminate();

protected:
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

public:
    void rtc_transform_init(u64 transform_id, SerializedTransferRecord options_record);
    void rtc_transform_teardown(u64 transform_id);
    void rtc_transform_encoded_audio_frame(u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number);

    using TransformFrameWrittenCallback = AK::Function<void(ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number)>;
    void register_transform_frame_callback(u64 transform_id, TransformFrameWrittenCallback callback);
    void unregister_transform_frame_callback(u64 transform_id);

private:
    WorkerAgentParent(URL::URL, WorkerOptions const&, GC::Ptr<MessagePort> outside_port,
        GC::Ref<EnvironmentSettingsObject> outside_settings, GC::Ref<DOM::EventTarget> worker_event_target,
        AgentType);

    void start();
    void forget_agent();
    void dispatch_error_event();
    void dispatch_worker_exception(Utf16String message, Utf16String filename, u32 lineno, u32 colno);
    void report_content_security_policy_violation(u64 policy_index, String effective_directive, URL::URL resource);

    static WorkerAgentOwnerToken next_owner_token();

    WorkerOptions m_worker_options;
    AgentType m_agent_type { AgentType::DedicatedWorker };
    URL::URL m_url;
    WorkerAgentId m_agent_id { 0 };
    WorkerAgentOwnerToken m_owner_token { 0 };

    GC::Ptr<MessagePort> m_message_port;
    GC::Ptr<MessagePort> m_outside_port;
    GC::Ref<EnvironmentSettingsObject> m_outside_settings;
    GC::Ref<DOM::EventTarget> m_worker_event_target;

    HashMap<u64, TransformFrameWrittenCallback> m_transform_frame_callbacks;
};

}
