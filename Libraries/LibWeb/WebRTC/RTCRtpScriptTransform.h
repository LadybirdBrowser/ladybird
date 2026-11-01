/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RTCRtpScriptTransform.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebRTC {

using Bindings::WorkerAndParameters;

class RTCRtpScriptTransform final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(RTCRtpScriptTransform, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(RTCRtpScriptTransform);

public:
    static WebIDL::ExceptionOr<GC::Ref<RTCRtpScriptTransform>> construct_impl(JS::Realm&,
        Variant<GC::Ref<HTML::Worker>, WorkerAndParameters> const& worker_or_parameters,
        Optional<JS::Value> options,
        Optional<GC::RootVector<GC::Ref<JS::Object>>> const& transfer);

    virtual ~RTCRtpScriptTransform() override;

    GC::Ptr<HTML::Worker> worker() const { return m_worker; }
    u64 transform_id() const { return m_transform_id; }

    // Send a frame to the worker over IPC. Frames written to the transformer's writable
    // return to the parent through on_frame_written.
    void enqueue_encoded_audio_frame(ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number);

    using FrameWrittenCallback = AK::Function<void(ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number)>;
    void set_on_frame_written(FrameWrittenCallback callback) { m_on_frame_written = move(callback); }
    bool has_frame_written_callback() const { return static_cast<bool>(m_on_frame_written); }

    bool has_owner() const { return m_has_owner; }
    void set_has_owner() { m_has_owner = true; }

    // Releases the transformer in the worker once the transform is no longer attached to a sender or receiver.
    void detach();

private:
    explicit RTCRtpScriptTransform();
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<HTML::Worker> m_worker;
    bool m_use_sframe { false };
    u64 m_transform_id { 0 };
    bool m_callback_registered { false };
    bool m_has_owner { false };
    bool m_detached { false };

    FrameWrittenCallback m_on_frame_written;
};

}
