/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGC/Heap.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Worker.h>
#include <LibWeb/HTML/WorkerAgentParent.h>
#include <LibWeb/WebRTC/RTCRtpScriptTransform.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCRtpScriptTransform);

// https://w3c.github.io/webrtc-encoded-transform/#dom-rtcrtpscripttransform-rtcrtpscripttransform
WebIDL::ExceptionOr<GC::Ref<RTCRtpScriptTransform>> RTCRtpScriptTransform::construct_impl(JS::Realm& realm,
    Variant<GC::Ref<HTML::Worker>, WorkerAndParameters> const& worker_or_parameters,
    Optional<JS::Value> options,
    Optional<GC::RootVector<GC::Ref<JS::Object>>> const& transfer)
{
    // 1. Let worker be undefined.
    GC::Ptr<HTML::Worker> worker;

    // 2. Let useSFrame be undefined.
    bool use_sframe = false;

    // 3. If workerOrWorkerAndParameters is a Worker object, set worker to workerOrWorkerAndParameters
    //    and useSFrame to false.
    // 4. Otherwise, run the following substeps:
    //    4.1. Set worker to workerOrWorkerAndParameters["worker"].
    //    4.2. Set useSFrame to true if workerOrWorkerAndParameters["type"] is "sframe" and false otherwise.
    worker_or_parameters.visit(
        [&](GC::Ref<HTML::Worker> const& bare_worker) {
            worker = bare_worker;
            use_sframe = false;
        },
        [&](WorkerAndParameters const& wap) {
            worker = wap.worker;
            use_sframe = wap.type == Bindings::RTCRtpScriptTransformType::Sframe;
        });

    auto transform = GC::Heap::the().allocate<RTCRtpScriptTransform>();
    // 5. Initialize this's internal slot as follows: [[worker]] worker
    transform->m_worker = worker;
    // 6. Initialize this.[[useSFrame]] to useSFrame.
    transform->m_use_sframe = use_sframe;

    // 7. Let serializedOptions be the result of StructuredSerializeWithTransfer(options, transfer).
    GC::RootVector<GC::Ref<JS::Object>> transfer_objects;
    if (transfer.has_value()) {
        for (auto const& object : *transfer)
            transfer_objects.append(object);
    }
    auto serialized_options = TRY(HTML::structured_serialize_with_transfer(realm, options.value_or(JS::js_undefined()), transfer_objects));

    // 8. Queue a global task on the DOM manipulation task source with worker's WorkerGlobalScope to:
    //    8.1. Deserialize serializedOptions.
    //    8.2. Create an RTCRtpScriptTransformer with the deserialized options.
    //    8.3. Fire an event named rtctransform using RTCTransformEvent on the transformer's
    //         relevant global object.
    // IPC delivers serializedOptions verbatim to ConnectionFromClient::rtc_transform_init,
    // which runs steps 8.1–8.3 in the worker process's realm.
    static u64 next_transform_id = 1;
    transform->m_transform_id = next_transform_id++;
    auto agent = transform->m_worker ? transform->m_worker->agent() : nullptr;
    if (agent)
        agent->rtc_transform_init(transform->m_transform_id, move(serialized_options));

    return transform;
}

RTCRtpScriptTransform::RTCRtpScriptTransform() = default;

RTCRtpScriptTransform::~RTCRtpScriptTransform() = default;

void RTCRtpScriptTransform::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_worker);
}

void RTCRtpScriptTransform::detach()
{
    if (m_detached)
        return;
    m_detached = true;
    m_on_frame_written = {};
    if (auto agent = m_worker ? m_worker->agent() : nullptr)
        agent->rtc_transform_teardown(m_transform_id);
}

void RTCRtpScriptTransform::enqueue_encoded_audio_frame(ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number)
{
    auto agent = m_worker ? m_worker->agent() : nullptr;
    if (!agent || m_detached)
        return;
    dbgln_if(WEBRTC_DEBUG, "RTCRtpScriptTransform::enqueue_encoded_audio_frame id={} ssrc={} seq={} len={}", m_transform_id, ssrc, sequence_number, payload.size());
    if (!m_callback_registered) {
        agent->register_transform_frame_callback(m_transform_id, [self = GC::Weak { *this }](ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number) {
            if (!self || !self->m_on_frame_written)
                return;
            self->m_on_frame_written(move(payload), ssrc, payload_type, rtp_timestamp, sequence_number);
        });
        m_callback_registered = true;
    }
    agent->rtc_transform_encoded_audio_frame(m_transform_id, move(payload), ssrc, payload_type, rtp_timestamp, sequence_number);
}

}
