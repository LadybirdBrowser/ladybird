/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebRTC/RTCRtpScriptTransformer.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCRtpScriptTransformer);

// https://w3c.github.io/webrtc-encoded-transform/#create-an-rtcrtpscripttransformer
WebIDL::ExceptionOr<GC::Ref<RTCRtpScriptTransformer>> RTCRtpScriptTransformer::create(JS::Realm& realm, JS::Value options)
{
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(realm.global_object());
    VERIFY(global_scope);
    auto transformer = GC::Heap::the().allocate<RTCRtpScriptTransformer>(global_scope->this_impl());

    // 1. Let transformer be a new RTCRtpScriptTransformer with:
    //    [[frameSource]] = undefined (set later via set_write_encoded_data_algorithm)
    //    [[options]] = options
    //    [[readable]] = a new ReadableStream
    //    [[writable]] = a new WritableStream
    //    [[lastReceivedFrameCounter]] = 0
    //    [[lastEnqueuedFrameCounter]] = 0
    transformer->m_options = options;

    // 2. Set up transformer.[[readable]]. The readEncodedData algorithm provides
    //    encoded frames to it. In our model, frames are pushed in via
    //    enqueue_encoded_frame from the upstream [[frameSource]] (the depacketizer
    //    on the parent side, plumbed through IPC), so the pull is a no-op.
    // NB: Streams only lets C++ set up a ReadableStream of arbitrary chunks as the readable side of a TransformStream,
    //     so frames are enqueued into one whose writable side nothing writes to.
    auto readable_transform = GC::create_function(realm.heap(), [transformer](JS::Value) -> GC::Ref<WebIDL::Promise> {
        return WebIDL::create_resolved_promise(transformer->relevant_realm(), JS::js_undefined());
    });
    transformer->m_readable_source = Streams::TransformStream::create(realm);
    transformer->m_readable_source->set_up(realm, readable_transform);
    transformer->m_readable = transformer->m_readable_source->readable();

    // 3. Let writeAlgorithm be an action that runs writeEncodedData with this as
    //    parameter and frame as input, given frame.
    auto write_algorithm = GC::create_function(realm.heap(), [transformer](JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
        // writeEncodedData step: hand the transformed frame to [[frameSource]] for
        // continued pipeline processing (decode, playback, or send-out for senders).
        if (transformer->m_write_encoded_data)
            transformer->m_write_encoded_data->function()(chunk);
        return WebIDL::create_resolved_promise(transformer->relevant_realm(), JS::js_undefined());
    });
    // 4. Set up transformer.[[writable]] with its writeAlgorithm set to writeAlgorithm
    //    and its highWaterMark set to Infinity.
    transformer->m_writable = Streams::WritableStream::create(realm);
    transformer->m_writable->set_up(realm, write_algorithm, AK::Infinity<double>);

    // 5. Return transformer.
    return transformer;
}

JS::Realm& RTCRtpScriptTransformer::relevant_realm() const
{
    return HTML::relevant_realm(HTML::relevant_window_or_worker_global_scope(*m_global_object));
}

void RTCRtpScriptTransformer::set_write_encoded_data_algorithm(GC::Ptr<WriteEncodedDataAlgorithm> algorithm)
{
    m_write_encoded_data = algorithm;
}

WebIDL::ExceptionOr<void> RTCRtpScriptTransformer::enqueue_encoded_frame(JS::Value frame)
{
    if (!m_readable_source || !m_readable->is_readable())
        return {};
    // Encoded audio is real-time data. Bound unread frames to 200 ms at the
    // normal 20 ms packet duration instead of replaying an ever-growing backlog.
    // NB: The readable side of a TransformStream has a highWaterMark of 0, so its desired size is minus its queue size.
    if (auto desired_size = m_readable->desired_size(); desired_size.has_value() && *desired_size <= -10)
        return {};
    ++m_last_received_frame_counter;
    TRY(m_readable_source->enqueue(frame));
    ++m_last_enqueued_frame_counter;
    return {};
}

void RTCRtpScriptTransformer::set_onkeyframerequest(WebIDL::CallbackType*) { }
WebIDL::CallbackType* RTCRtpScriptTransformer::onkeyframerequest() { return nullptr; }

RTCRtpScriptTransformer::RTCRtpScriptTransformer(GC::Ref<DOM::EventTarget> relevant_global_object)
    : DOM::EventTarget()
    , m_global_object(relevant_global_object)
{
}

RTCRtpScriptTransformer::~RTCRtpScriptTransformer() = default;

void RTCRtpScriptTransformer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_object);
    visitor.visit(m_options);
    visitor.visit(m_readable_source);
    visitor.visit(m_readable);
    visitor.visit(m_writable);
    visitor.visit(m_write_encoded_data);
}

}
