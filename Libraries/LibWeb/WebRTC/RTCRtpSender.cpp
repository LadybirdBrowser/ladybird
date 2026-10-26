/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebRTC/RTCPeerConnection.h>
#include <LibWeb/WebRTC/RTCRtpReceiver.h>
#include <LibWeb/WebRTC/RTCRtpScriptTransform.h>
#include <LibWeb/WebRTC/RTCRtpSender.h>
#include <LibWeb/WebRTC/RTCRtpTransceiver.h>
#include <LibWeb/WebRTC/RTCSFrameSenderTransform.h>
#include <LibWeb/WebRTC/WebRTCAgent.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCRtpSender);

GC::Ref<RTCRtpSender> RTCRtpSender::create(GC::Ref<RTCPeerConnection> connection, u64 sender_id, u32 ssrc)
{
    return GC::Heap::the().allocate<RTCRtpSender>(connection, sender_id, ssrc);
}

RTCRtpSender::RTCRtpSender(GC::Ref<RTCPeerConnection> connection, u64 sender_id, u32 ssrc)
    : m_connection(connection)
    , m_sender_id(sender_id)
    , m_ssrc(ssrc)
{
    // Seed [[SendEncodings]] with a single encoding mirroring the wire-side SSRC. This is what consumers like
    // Discord's EncryptionWorker read out of `getParameters().encodings[0].ssrc` to register their userId mapping.
    RTCRtpEncodingParameters encoding;
    encoding.ssrc = m_ssrc;
    m_send_encodings.append(move(encoding));
}

void RTCRtpSender::set_ssrc(u32 ssrc)
{
    m_ssrc = ssrc;
    m_ssrc_confirmed = true;
    if (m_send_encodings.is_empty())
        m_send_encodings.append({});
    m_send_encodings[0].ssrc = m_ssrc;
    // Rebuild getParameters() with the updated SSRC.
    m_last_returned_parameters = {};
}

RTCRtpSender::~RTCRtpSender() = default;

JS::Realm& RTCRtpSender::relevant_realm() const
{
    return m_connection->relevant_realm();
}

void RTCRtpSender::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_connection);
    visitor.visit(m_track);
    visitor.visit(m_transform);
}

void RTCRtpSender::set_track(GC::Ptr<MediaCapture::MediaStreamTrack> track)
{
    m_track = track;
    m_connection->on_sender_track_changed(*this);
}

RTCRtpSenderTransform RTCRtpSender::transform() const
{
    if (!m_transform)
        return Empty {};
    if (auto* script_transform = as_if<RTCRtpScriptTransform>(*m_transform))
        return GC::Ref { *script_transform };
    if (auto* sframe_transform = as_if<RTCSFrameSenderTransform>(*m_transform))
        return GC::Ref { *sframe_transform };
    return Empty {};
}

GC::Ptr<RTCRtpScriptTransform> RTCRtpSender::script_transform()
{
    if (!m_transform)
        return nullptr;
    return as_if<RTCRtpScriptTransform>(*m_transform);
}

// https://w3c.github.io/webrtc-encoded-transform/#dom-rtcrtpsender-transform
WebIDL::ExceptionOr<void> RTCRtpSender::set_transform(RTCRtpSenderTransform value)
{
    // NB: A script transform belongs to the first sender or receiver it's set on, even once it's replaced there.
    auto* new_script_transform = value.get_pointer<GC::Ref<RTCRtpScriptTransform>>();
    if (new_script_transform && (*new_script_transform)->has_owner())
        return WebIDL::InvalidStateError::create("The transform is already in use"_utf16);
    if (new_script_transform)
        (*new_script_transform)->set_has_owner();

    if (auto* old_script_transform = m_transform ? as_if<RTCRtpScriptTransform>(*m_transform) : nullptr)
        old_script_transform->detach();
    m_transform = value.visit(
        [](Empty) -> GC::Ptr<Bindings::Wrappable> { return nullptr; },
        [](GC::Ref<RTCSFrameSenderTransform> const& t) -> GC::Ptr<Bindings::Wrappable> { return t.ptr(); },
        [](GC::Ref<RTCRtpScriptTransform> const& t) -> GC::Ptr<Bindings::Wrappable> { return t.ptr(); });
    dbgln_if(WEBRTC_DEBUG, "RTCRtpSender::set_transform: transform set={}", m_transform != nullptr);
    m_connection->on_sender_transform_changed(*this);
    return {};
}

GC::Ref<WebIDL::Promise> RTCRtpSender::replace_track(GC::Ptr<MediaCapture::MediaStreamTrack> with_track)
{
    auto& realm = relevant_realm();
    auto promise = WebIDL::create_promise(realm);
    if (m_connection->is_closed()) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16));
        return promise;
    }
    for (auto const& transceiver : m_connection->get_transceivers()) {
        if (transceiver->sender().ptr() != this)
            continue;
        if (transceiver->is_stopping() || transceiver->is_stopped()) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create("Transceiver is stopped"_utf16));
            return promise;
        }
        if (with_track && with_track->track_kind() != transceiver->receiver()->track()->track_kind()) {
            WebIDL::reject_promise_with_exception(realm, promise, WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Replacement track has a different kind"_utf16 });
            return promise;
        }
    }
    set_track(with_track);
    WebIDL::resolve_promise(realm, promise, JS::js_undefined());
    return promise;
}

// https://w3c.github.io/webrtc-pc/#dom-rtcrtpsender-setparameters
GC::Ref<WebIDL::Promise> RTCRtpSender::set_parameters(RTCRtpSendParameters const& parameters, RTCSetParameterOptions const&)
{
    // 1. Let parameters be the method's first argument.
    // 2. Let sender be the RTCRtpSender object on which setParameters is invoked.

    // FIXME: 3. Let transceiver be the RTCRtpTransceiver object associated with sender (i.e. sender is transceiver.[[Sender]]).
    // FIXME: 4. If transceiver.[[Stopping]] is true, return a promise rejected with a newly created InvalidStateError.

    // FIXME: 5. If sender.[[LastReturnedParameters]] is null, return a promise rejected with a newly created InvalidStateError.
    //        Real browsers strictly require a getParameters() round-trip before each setParameters(); we don't
    //        enforce that yet because some apps (e.g. Discord's setTransceiverEncodingParameters) call
    //        setParameters early in setup before any getParameters.

    // FIXME: 6. Validate parameters by running the setParameters validation steps.

    // 7. Let p be a new promise.
    auto p = WebIDL::create_promise(relevant_realm());

    // 8. In parallel, configure the media stack to use parameters to transmit sender.[[SenderTrack]].
    //    FIXME: actually wire `parameters` into the media stack (bitrate caps, encoding rid, etc).
    HTML::queue_a_task(HTML::Task::Source::Networking, nullptr, nullptr, GC::create_function(heap(), [this, p, parameters] {
        // Promise resolution calls into JS and needs a running execution context on the VM stack; tasks don't
        // come with one by default.
        auto& realm = relevant_realm();
        HTML::TemporaryExecutionContext context(realm);
        // 8.1. If the media stack is successfully configured with parameters, queue a task to run the following steps:
        //   8.1.1. Set sender.[[LastReturnedParameters]] to null.
        m_last_returned_parameters = {};
        //   8.1.2. Set sender.[[SendEncodings]] to parameters.encodings.
        m_send_encodings = parameters.encodings;
        //   8.1.3. Resolve p with undefined.
        WebIDL::resolve_promise(realm, p, JS::js_undefined());
        // 8.2. If any error occurred while configuring the media stack, queue a task to run the following steps:
        //   8.2.1. If an error occurred due to hardware resources not being available, reject p with a newly
        //          created RTCError whose errorDetail is set to "hardware-encoder-not-available" and abort
        //          these steps.
        //   8.2.2. If an error occurred due to a hardware encoder not supporting parameters, reject p with a
        //          newly created RTCError whose errorDetail is set to "hardware-encoder-error" and abort these
        //          steps.
        //   8.2.3. For all other errors, reject p with a newly created OperationError.
    }));

    // 9. Return p.
    return p;
}

// https://w3c.github.io/webrtc-pc/#dom-rtcrtpsender-getparameters
RTCRtpSendParameters RTCRtpSender::get_parameters()
{
    // Applications can read encodings immediately after addTrack, before the
    // asynchronous acknowledgement. DAVE must register the actual wire SSRC.
    if (!m_ssrc_confirmed && !m_connection->is_closed()) {
        if (auto* client = WebRTCAgent::the().existing_client()) {
            auto result = client->try_get_sender_ssrc(m_sender_id);
            if (!result.is_error() && result.value() != 0)
                set_ssrc(result.value());
        }
    }
    // 1. Let sender be the RTCRtpSender object on which the getter was invoked.

    // 2. If sender.[[LastReturnedParameters]] is not null, return sender.[[LastReturnedParameters]], and abort these steps.
    if (m_last_returned_parameters.has_value())
        return m_last_returned_parameters.value();

    // 3. Let result be a new RTCRtpSendParameters dictionary constructed as follows:
    RTCRtpSendParameters result;
    //    transactionId is set to a new unique identifier.
    // FIXME: assign a fresh transaction id.
    //    encodings is set to the value of the [[SendEncodings]] internal slot.
    result.encodings = m_send_encodings;
    //    The headerExtensions sequence is populated based on the header extensions that have been negotiated for sending.
    // FIXME: populate from negotiated header extensions.
    //    codecs is set to the value of the [[SendCodecs]] internal slot.
    // FIXME: populate from [[SendCodecs]].
    //    rtcp.cname is set to the CNAME of the associated RTCPeerConnection. rtcp.reducedSize is set to true if
    //        reduced-size RTCP has been negotiated for sending, and false otherwise.
    // FIXME: populate rtcp.cname and rtcp.reducedSize from the connection.

    // 4. Set sender.[[LastReturnedParameters]] to result.
    m_last_returned_parameters = result;

    // 5. Queue a task that sets sender.[[LastReturnedParameters]] to null.
    HTML::queue_a_task(HTML::Task::Source::Networking, nullptr, nullptr, GC::create_function(heap(), [this] {
        m_last_returned_parameters = {};
    }));

    // 6. Return result.
    return result;
}

}
