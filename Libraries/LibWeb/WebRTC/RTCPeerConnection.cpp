/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibAudio/PlaybackStream.h>
#include <LibCore/EventLoop.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/Realm.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/FFmpeg/FFmpegAudioEncoder.h>
#include <LibMediaCodecs/AudioBlock.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibWeb/Bindings/RTCPeerConnection.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Infra/JSON.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebRTC/RTCDataChannel.h>
#include <LibWeb/WebRTC/RTCDataChannelEvent.h>
#include <LibWeb/WebRTC/RTCEncodedAudioFrame.h>
#include <LibWeb/WebRTC/RTCIceCandidate.h>
#include <LibWeb/WebRTC/RTCPeerConnection.h>
#include <LibWeb/WebRTC/RTCPeerConnectionIceEvent.h>
#include <LibWeb/WebRTC/RTCRtpReceiver.h>
#include <LibWeb/WebRTC/RTCRtpScriptTransform.h>
#include <LibWeb/WebRTC/RTCRtpSender.h>
#include <LibWeb/WebRTC/RTCRtpTransceiver.h>
#include <LibWeb/WebRTC/RTCSctpTransport.h>
#include <LibWeb/WebRTC/RTCSessionDescription.h>
#include <LibWeb/WebRTC/RTCStatsReport.h>
#include <LibWeb/WebRTC/RTCTrackEvent.h>
#include <LibWeb/WebRTC/WebRTCAgent.h>
#include <LibWebRTCClient/Client.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCPeerConnection);

WebIDL::ExceptionOr<GC::Ref<RTCPeerConnection>> RTCPeerConnection::create_for_constructor(JS::Object& relevant_global_object, RTCConfiguration const& configuration)
{
    if (!WebRTCAgent::the().is_ready())
        return WebIDL::OperationError::create("Unable to start the WebRTC helper"_utf16);
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(relevant_global_object);
    VERIFY(global_scope);
    auto connection = GC::Heap::the().allocate<RTCPeerConnection>(global_scope->this_impl(), configuration);
    JsonArray servers;
    for (auto const& server : configuration.ice_servers) {
        JsonArray urls;
        server.urls.visit(
            [&](Utf16String const& url) { urls.must_append(url.to_utf8()); },
            [&](auto const& entries) { for (auto const& url : entries) urls.must_append(url.to_utf8()); });
        JsonObject entry;
        entry.set("urls"sv, move(urls));
        entry.set("username"sv, server.username.value_or(Utf16String {}).to_utf8());
        entry.set("credential"sv, server.credential.value_or(Utf16String {}).to_utf8());
        servers.must_append(move(entry));
    }
    JsonObject config;
    config.set("ice_servers"sv, move(servers));
    config.set("ice_transport_policy"sv, idl_enum_to_string(configuration.ice_transport_policy).to_utf8());
    config.set("bundle_policy"sv, idl_enum_to_string(configuration.bundle_policy).to_utf8());
    config.set("ice_candidate_pool_size"sv, configuration.ice_candidate_pool_size);
    // Like other browsers, only reveal local addresses to pages that may capture audio, which could tell where the
    // user is anyway. Everyone else gets mDNS names in their place.
    // https://datatracker.ietf.org/doc/html/draft-ietf-mmusic-mdns-ice-candidates#section-3.1
    PermissionsAPI::PermissionDescriptor microphone_permission { "microphone"_utf16 };
    auto may_capture = PermissionsAPI::permission_state(microphone_permission) == PermissionsAPI::PermissionState::Granted;
    config.set("host_candidates"sv, may_capture ? "exposed"sv : "obfuscated"sv);
    if (!configuration.certificates.is_empty())
        return WebIDL::NotSupportedError::create("Custom RTC certificates are not supported"_utf16);
    auto result = WebRTCAgent::the().client()->try_create_peer_connection(connection->m_pc_id, config.serialized());
    if (result.is_error()) {
        connection->m_is_closed = true;
        return WebIDL::OperationError::create("WebRTC helper disconnected during construction"_utf16);
    }
    if (!result.value().is_empty()) {
        connection->m_is_closed = true;
        return WebIDL::OperationError::create(Utf16String::from_utf8(result.value()));
    }
    WebRTCAgent::the().register_peer_connection(connection->m_pc_id, connection);
    return connection;
}

RTCPeerConnection::RTCPeerConnection(GC::Ref<DOM::EventTarget> relevant_global_object, RTCConfiguration configuration)
    : DOM::EventTarget()
    , m_global_object(relevant_global_object)
    , m_configuration(move(configuration))
{
    auto& agent = WebRTCAgent::the();
    m_pc_id = agent.next_pc_id();
}

void RTCPeerConnection::finalize()
{
    auto& agent = WebRTCAgent::the();
    if (!m_is_closed) {
        if (auto* client = agent.existing_client())
            client->async_close_peer_connection(m_pc_id);
    }
    agent.unregister_peer_connection(m_pc_id);
    Base::finalize();
}

JS::Realm& RTCPeerConnection::relevant_realm() const
{
    return HTML::relevant_realm(HTML::relevant_window_or_worker_global_scope(*m_global_object));
}

JS::Object& RTCPeerConnection::relevant_global_object() const
{
    return HTML::relevant_global_object(HTML::relevant_window_or_worker_global_scope(*m_global_object));
}

void RTCPeerConnection::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_object);
    visitor.visit(m_sctp);
    visitor.visit(m_last_operation);
    visitor.visit(m_transceivers);
    visitor.visit(m_data_channels);
    visitor.visit(m_data_channels_by_id);
    visitor.visit(m_current_local_description);
    visitor.visit(m_pending_local_description);
    visitor.visit(m_current_remote_description);
    visitor.visit(m_pending_remote_description);
    visitor.visit(m_remote_streams);
    visitor.visit(m_remote_receivers_by_id);
    for (auto& [_, pipeline] : m_outgoing_audio_pipelines)
        visitor.visit(pipeline->track);
}

GC::Ref<WebIDL::Promise> RTCPeerConnection::chain_operation(GC::Ref<WebIDL::ReactionSteps> operation)
{
    if (m_is_closed)
        return WebIDL::create_rejected_promise(relevant_realm(), WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16));
    auto previous = m_last_operation;
    if (!previous)
        previous = WebIDL::create_resolved_promise(relevant_realm(), JS::js_undefined());
    m_last_operation = WebIDL::react_to_promise(*previous, operation, operation);
    return *m_last_operation;
}

void RTCPeerConnection::on_helper_died()
{
    HTML::TemporaryExecutionContext context(relevant_realm());
    for (auto& [_, promise] : m_pending_void_requests)
        WebIDL::reject_promise(relevant_realm(), *promise, WebIDL::OperationError::create("WebRTC helper disconnected"_utf16));
    for (auto& [_, promise] : m_pending_description_requests)
        WebIDL::reject_promise(relevant_realm(), *promise, WebIDL::OperationError::create("WebRTC helper disconnected"_utf16));
    m_pending_void_requests.clear();
    m_pending_description_requests.clear();
    m_pending_description_payloads.clear();
    m_held_ice_events.clear();
    close();
    dispatch_event(DOM::Event::create(relevant_global_object(), HTML::EventNames::connectionstatechange));
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-close
void RTCPeerConnection::close()
{
    // When the close method is invoked, the user agent MUST run the following steps:
    // 1. Let connection be the RTCPeerConnection object on which the method was invoked.
    // 2. close the connection with connection and the value false.
    close_the_connection_algorithm(false);
}

void RTCPeerConnection::close_for_document_destruction()
{
    close_the_connection_algorithm(true);
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-close
void RTCPeerConnection::close_the_connection_algorithm(bool disappear)
{
    // The close the connection algorithm given a connection and a disappear boolean, is as follows:
    // 1. If connection.[[IsClosed]] is true, abort these steps.
    if (m_is_closed)
        return;
    // 2.Set connection.[[IsClosed]] to true.
    m_is_closed = true;
    // 3. Set connection.[[SignalingState]] to "closed". This does not fire any event.
    m_signaling_state = Bindings::RTCSignalingState::Closed;
    // 4. Let transceivers be the result of executing the CollectTransceivers algorithm...
    auto transceivers = collect_transceivers();
    // 4... For every RTCRtpTransceiver transceiver in transceivers, run the following steps:
    for (auto& transceiver : transceivers) {
        // 4.1. If transceiver.[[Stopped]] is true, abort these sub steps.
        if (transceiver->is_stopped())
            continue;
        // 4.2. Stop the RTCRtpTransceiver with transceiver and disappear.
        transceiver->stop(disappear);
    }
    auto& agent = WebRTCAgent::the();
    if (auto* client = agent.existing_client())
        client->async_close_peer_connection(m_pc_id);
    for (auto& [id, channel] : m_data_channels_by_id) {
        channel->set_ready_state(Bindings::RTCDataChannelState::Closed);
        agent.unregister_data_channel(id);
    }
    for (auto& [_, pipeline] : m_outgoing_audio_pipelines) {
        if (pipeline->track && pipeline->sink)
            pipeline->track->remove_audio_sink(*pipeline->sink);
    }
    m_outgoing_audio_pipelines.clear();
    m_last_operation = nullptr;
    m_pending_void_requests.clear();
    m_pending_description_requests.clear();
    m_pending_description_payloads.clear();
    m_held_ice_events.clear();
    for (auto& [_, playback] : m_receiver_audio_playbacks) {
        if (playback->playback_stream)
            (void)playback->playback_stream->discard_buffer_and_suspend();
    }
    m_receiver_audio_playbacks.clear();
    agent.unregister_peer_connection(m_pc_id);
    // 6. If connection.[[SctpTransport]] is not null...
    if (m_sctp) {
        // TODO: 6. ...tear down the underlying SCTP association by sending an SCTP ABORT chunk and set the [[SctpTransportState]] to "closed".
    }
    // TODO: 7. Set the [[DtlsTransportState]] slot of each of connection's RTCDtlsTransports to "closed".
    // TODO: 8. Destroy connection's ICE Agent, abruptly ending any active ICE processing and releasing any relevant resources (e.g. TURN permissions).
    // TODO: 9. Set the [[IceTransportState]] slot of each of connection's RTCIceTransports to "closed".
    // 10. Set connection.[[IceConnectionState]] to "closed". This does not fire any event.
    m_ice_connection_state = Bindings::RTCIceConnectionState::Closed;
    // 11. Set connection.[[ConnectionState]] to "closed". This does not fire any event.
    m_connection_state = Bindings::RTCPeerConnectionState::Closed;
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-createoffer
GC::Ref<WebIDL::Promise> RTCPeerConnection::create_offer(RTCOfferOptions const& options)
{
    return chain_operation(GC::create_function(heap(), [this, options](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        return create_offer_impl(options)->promise();
    }));
}

GC::Ref<WebIDL::Promise> RTCPeerConnection::create_offer_impl(RTCOfferOptions const& options)
{
    // 1. Let connection be the RTCPeerConnection object on which the method was invoked.
    auto& realm = relevant_realm();
    // 2. If connection.[[IsClosed]] is true, return a promise rejected with a newly created InvalidStateError.
    if (m_is_closed) {
        auto promise = WebIDL::create_promise(realm);
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16));
        return promise;
    }
    // FIXME: 3. Return the result of chaining the result of creating an offer with connection to connection's operations chain.
    m_restart_ice |= options.ice_restart;
    return create_an_offer();
}

// https://www.w3.org/TR/webrtc/#create-an-offer
GC::Ref<WebIDL::Promise> RTCPeerConnection::create_an_offer()
{
    auto& realm = relevant_realm();
    // 1. If connection.[[SignalingState]] is neither "stable" nor "have-local-offer", return a promise rejected with a newly created InvalidStateError.
    if (m_signaling_state != Bindings::RTCSignalingState::Stable && m_signaling_state != Bindings::RTCSignalingState::HaveLocalOffer) {
        auto rejected = WebIDL::create_promise(realm);
        WebIDL::reject_promise(realm, rejected, WebIDL::InvalidStateError::create("RTCPeerConnection is not in a state to create an offer"_utf16));
        return rejected;
    }
    // 2. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);
    // 3. In parallel, begin the in-parallel steps to create an offer given connection and p.
    //    Service-side equivalents (in parallel + the final-step queued task) are run by the rust
    //    WebRTCClient; result arrives via on_create_offer_result.
    auto& agent = WebRTCAgent::the();
    auto request_id = agent.next_request_id();
    m_pending_description_requests.set(request_id, p);
    if (auto* client = agent.client())
        client->async_create_offer(m_pc_id, request_id, m_restart_ice);
    // 4. Return p.
    return p;
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-createanswer
GC::Ref<WebIDL::Promise> RTCPeerConnection::create_answer(RTCAnswerOptions const& options)
{
    return chain_operation(GC::create_function(heap(), [this, options](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        return create_answer_impl(options)->promise();
    }));
}

GC::Ref<WebIDL::Promise> RTCPeerConnection::create_answer_impl(RTCAnswerOptions const&)
{
    auto& realm = relevant_realm();
    // 1. Let connection be the RTCPeerConnection object on which the method was invoked.
    // 2. If connection.[[IsClosed]] is true, return a promise rejected with a newly created InvalidStateError.
    if (m_is_closed) {
        auto rejected = WebIDL::create_promise(realm);
        WebIDL::reject_promise(realm, rejected, WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16));
        return rejected;
    }
    // FIXME: 3. Return the result of chaining the result of creating an answer with connection to connection's operations chain.
    return create_an_answer();
}

// https://www.w3.org/TR/webrtc/#create-an-answer
GC::Ref<WebIDL::Promise> RTCPeerConnection::create_an_answer()
{
    auto& realm = relevant_realm();
    // 1. If connection.[[SignalingState]] is neither "have-remote-offer" nor "have-local-pranswer", return a promise rejected with a newly created InvalidStateError.
    if (m_signaling_state != Bindings::RTCSignalingState::HaveRemoteOffer && m_signaling_state != Bindings::RTCSignalingState::HaveLocalPranswer) {
        auto rejected = WebIDL::create_promise(realm);
        WebIDL::reject_promise(realm, rejected, WebIDL::InvalidStateError::create("RTCPeerConnection is not in a state to create an answer"_utf16));
        return rejected;
    }
    // 2. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);
    // 3. In parallel, begin the in-parallel steps to create an answer given connection and p.
    //    Service-side equivalents (in parallel + the final-step queued task) are run by the rust
    //    WebRTCClient; result arrives via on_create_answer_result.
    auto& agent = WebRTCAgent::the();
    auto request_id = agent.next_request_id();
    m_pending_description_requests.set(request_id, p);
    if (auto* client = agent.client())
        client->async_create_answer(m_pc_id, request_id);
    // 4. Return p.
    return p;
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-setlocaldescription
GC::Ref<WebIDL::Promise> RTCPeerConnection::set_local_description(RTCLocalSessionDescriptionInit const& description)
{
    return chain_operation(GC::create_function(heap(), [this, description](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        return set_local_description_impl(description)->promise();
    }));
}

GC::Ref<WebIDL::Promise> RTCPeerConnection::set_local_description_impl(RTCLocalSessionDescriptionInit const& description)
{
    if (m_is_closed)
        return WebIDL::create_rejected_promise(relevant_realm(), WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16));
    auto& realm = relevant_realm();
    // 1. Let description be the method's first argument.
    // 2. Let connection be the RTCPeerConnection object on which the method was invoked.
    // 3. Let sdp be description.sdp.
    auto sdp = description.sdp;
    // FIXME: 4. Return the result of chaining the following steps to connection's operations chain:
    // 4.1. Let type be description.type if present, or "offer" if not present and connection.[[SignalingState]] is either "stable", "have-local-offer", or "have-remote-pranswer"; otherwise "answer".
    Bindings::RTCSdpType type;
    if (description.type.has_value()) {
        type = *description.type;
    } else if (m_signaling_state == Bindings::RTCSignalingState::Stable
        || m_signaling_state == Bindings::RTCSignalingState::HaveLocalOffer
        || m_signaling_state == Bindings::RTCSignalingState::HaveRemotePranswer) {
        type = Bindings::RTCSdpType::Offer;
    } else {
        type = Bindings::RTCSdpType::Answer;
    }
    // 4.2. If type is "offer", and sdp is not the empty string and not equal to connection.[[LastCreatedOffer]], then return a promise rejected with a newly created InvalidModificationError and abort these steps.
    if (type == Bindings::RTCSdpType::Offer && !sdp.is_empty() && sdp != m_last_created_offer) {
        auto rejected = WebIDL::create_promise(realm);
        WebIDL::reject_promise(realm, rejected, WebIDL::InvalidModificationError::create("Local description SDP does not match last created offer"_utf16));
        return rejected;
    }
    // 4.3. If type is "answer" or "pranswer", and sdp is not the empty string and not equal to connection.[[LastCreatedAnswer]], then return a promise rejected with a newly created InvalidModificationError and abort these steps.
    if ((type == Bindings::RTCSdpType::Answer || type == Bindings::RTCSdpType::Pranswer) && !sdp.is_empty() && sdp != m_last_created_answer) {
        auto rejected = WebIDL::create_promise(realm);
        WebIDL::reject_promise(realm, rejected, WebIDL::InvalidModificationError::create("Local description SDP does not match last created answer"_utf16));
        return rejected;
    }
    if (sdp.is_empty() && type != Bindings::RTCSdpType::Rollback) {
        auto generated = type == Bindings::RTCSdpType::Offer ? create_an_offer() : create_an_answer();
        return WebIDL::react_to_promise(generated, GC::create_function(heap(), [this, type](JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
            if (m_is_closed)
                return WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16);
            auto sdp_value = TRY(value.as_object().get("sdp"_utf16_fly_string));
            auto generated_sdp = TRY(sdp_value.to_utf16_string(relevant_realm().vm()));
            return set_a_local_description(type, generated_sdp)->promise();
        }),
            nullptr);
    }
    // 4.6. Return the result of setting the local session description indicated by {type, sdp}.
    return set_a_local_description(type, sdp);
}

// FIXME: https://www.w3.org/TR/webrtc/#set-the-rtcsessiondescription (set the local session description)
GC::Ref<WebIDL::Promise> RTCPeerConnection::set_a_local_description(Bindings::RTCSdpType type, Utf16String const& sdp)
{
    auto& realm = relevant_realm();
    auto p = WebIDL::create_promise(realm);
    auto& agent = WebRTCAgent::the();
    auto request_id = agent.next_request_id();
    m_pending_void_requests.set(request_id, p);
    m_pending_description_payloads.set(request_id, PendingDescription { type, sdp, true });
    if (auto* client = agent.client())
        client->async_set_local_description(m_pc_id, request_id, idl_enum_to_string(type).to_well_formed_utf8(), sdp.to_well_formed_utf8());
    return p;
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-setremotedescription
GC::Ref<WebIDL::Promise> RTCPeerConnection::set_remote_description(RTCSessionDescriptionInit const& description)
{
    return chain_operation(GC::create_function(heap(), [this, description](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        return set_remote_description_impl(description)->promise();
    }));
}

GC::Ref<WebIDL::Promise> RTCPeerConnection::set_remote_description_impl(RTCSessionDescriptionInit const& description)
{
    if (m_is_closed)
        return WebIDL::create_rejected_promise(relevant_realm(), WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16));
    // 1. Let description be the method's first argument.
    // 2. Let connection be the RTCPeerConnection object on which the method was invoked.
    // FIXME: 3. Return the result of chaining the following steps to connection's operations chain:
    // FIXME: 3.1. If description.type is "offer" and is invalid for the current connection.[[SignalingState]] as described in [RFC9429] (section 5.5. and section 5.6.), then run the following sub steps:
    //   FIXME: 3.1.1. Let p be the result of setting the local session description indicated by {type: "rollback"}.
    //   FIXME: 3.1.2. Return the result of reacting to p with a fulfillment step that sets the remote session description description, and abort these steps.
    // 3.2. Return the result of setting the remote session description description.
    return set_a_remote_description(description);
}

// FIXME: https://www.w3.org/TR/webrtc/#set-description (set the remote session description)
GC::Ref<WebIDL::Promise> RTCPeerConnection::set_a_remote_description(RTCSessionDescriptionInit const& description)
{
    auto& realm = relevant_realm();
    auto p = WebIDL::create_promise(realm);
    auto& agent = WebRTCAgent::the();
    auto request_id = agent.next_request_id();
    m_pending_void_requests.set(request_id, p);
    m_pending_description_payloads.set(request_id, PendingDescription { description.type, description.sdp, false });
    if (auto* client = agent.client())
        client->async_set_remote_description(m_pc_id, request_id, idl_enum_to_string(description.type).to_well_formed_utf8(), description.sdp.to_well_formed_utf8());
    return p;
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-localdescription
GC::Ptr<RTCSessionDescription> RTCPeerConnection::local_description() const
{
    // The localDescription attribute MUST return [[PendingLocalDescription]] if not null and otherwise [[CurrentLocalDescription]].
    if (m_pending_local_description)
        return m_pending_local_description;
    return m_current_local_description;
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-remotedescription
GC::Ptr<RTCSessionDescription> RTCPeerConnection::remote_description() const
{
    // The remoteDescription attribute MUST return [[PendingRemoteDescription]] if not null and otherwise [[CurrentRemoteDescription]].
    if (m_pending_remote_description)
        return m_pending_remote_description;
    return m_current_remote_description;
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-getreceivers
GC::RootVector<GC::Ref<RTCRtpReceiver>> RTCPeerConnection::get_receivers() const
{
    // 1. Let transceivers be the result of executing the CollectTransceivers algorithm.
    auto transceivers = collect_transceivers();
    // 2. Let receivers be a new empty sequence.
    GC::RootVector<GC::Ref<RTCRtpReceiver>> receivers;
    // 3. For each transceiver in transceivers,
    for (auto& transceiver : transceivers) {
        // 3.1. If transceiver.[[Stopped]] is false, add transceiver.[[Receiver]] to receivers.
        if (!transceiver->is_stopped())
            receivers.append(transceiver->receiver());
    }
    // 4. Return receivers.
    return receivers;
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-addicecandidate
GC::Ref<WebIDL::Promise> RTCPeerConnection::add_ice_candidate(RTCIceCandidateInit const& candidate)
{
    return chain_operation(GC::create_function(heap(), [this, candidate](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
        return add_ice_candidate_impl(candidate)->promise();
    }));
}

GC::Ref<WebIDL::Promise> RTCPeerConnection::add_ice_candidate_impl(RTCIceCandidateInit const& candidate)
{
    if (m_is_closed)
        return WebIDL::create_rejected_promise(relevant_realm(), WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16));
    auto& realm = relevant_realm();
    // 1. Let candidate be the method's argument.
    // 2. Let connection be the RTCPeerConnection object on which the method was invoked.
    // 3. If candidate.candidate is not an empty string and both candidate.sdpMid and candidate.sdpMLineIndex are null, return a promise rejected with a newly created TypeError.
    if (!candidate.candidate.is_empty() && !candidate.sdp_mid.has_value() && !candidate.sdp_m_line_index.has_value()) {
        auto rejected = WebIDL::create_promise(realm);
        WebIDL::reject_promise(realm, rejected, JS::TypeError::create(realm, "addIceCandidate: sdpMid and sdpMLineIndex are both null"_utf16));
        return rejected;
    }
    // FIXME: 4. Return the result of chaining the following steps to connection's operations chain:
    auto p = WebIDL::create_promise(realm);
    if (!remote_description()) {
        WebIDL::reject_promise(realm, p, WebIDL::InvalidStateError::create("Remote description is not set"_utf16));
        return p;
    }
    // FIXME: 4.2. If candidate.sdpMid is not null, run the following steps:
    //   FIXME: 4.2.1. If candidate.sdpMid is not equal to the mid of any media description in remoteDescription, return a promise rejected with a newly created OperationError.
    // FIXME: 4.3. Else, if candidate.sdpMLineIndex is not null, run the following steps:
    //   FIXME: 4.3.1. If candidate.sdpMLineIndex is equal to or larger than the number of media descriptions in remoteDescription, return a promise rejected with a newly created OperationError.
    // FIXME: 4.4. If either candidate.sdpMid or candidate.sdpMLineIndex indicate a media description in remoteDescription whose associated transceiver is stopped, return a promise resolved with undefined.
    // FIXME: 4.5. If candidate.usernameFragment is not null, and is not equal to any username fragment present in the corresponding media description of an applied remote description, return a promise rejected with a newly created OperationError.
    // 4.6. Let p be a new promise.
    // 4.7. In parallel, if the candidate is not administratively prohibited, add the ICE candidate
    //      candidate as described in [RFC9429] (section 4.1.19.). The service runs the in-parallel
    //      branch and queues the result task; it lands here via on_add_ice_candidate_result.
    auto& agent = WebRTCAgent::the();
    auto request_id = agent.next_request_id();
    m_pending_void_requests.set(request_id, p);
    if (auto* client = agent.client()) {
        client->async_add_ice_candidate(
            m_pc_id,
            request_id,
            candidate.candidate.to_well_formed_utf8(),
            candidate.sdp_mid.map([](Utf16String const& v) { return v.to_well_formed_utf8(); }),
            candidate.sdp_m_line_index.map([](u16 v) { return static_cast<u32>(v); }),
            candidate.username_fragment.map([](Utf16String const& v) { return v.to_well_formed_utf8(); }));
    }
    // 4.8. Return p.
    return p;
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-addtrack
WebIDL::ExceptionOr<GC::Ref<RTCRtpSender>> RTCPeerConnection::add_track(GC::Ref<MediaCapture::MediaStreamTrack> track, GC::Ref<MediaCapture::MediaStream> stream)
{
    GC::RootVector<GC::Ref<MediaCapture::MediaStream>> streams;
    streams.append(stream);
    return add_track(track, streams);
}

WebIDL::ExceptionOr<GC::Ref<RTCRtpSender>> RTCPeerConnection::add_track(GC::Ref<MediaCapture::MediaStreamTrack> track, GC::RootVector<GC::Ref<MediaCapture::MediaStream>> const& streams)
{
    // 1. Let connection be the RTCPeerConnection object on which this method was invoked.
    // 2. Let track be the MediaStreamTrack object indicated by the method's first argument.
    // 3. Let kind be track.kind.
    auto kind = track->track_kind();
    // 4. Let streams be a list of MediaStream objects constructed from the method's remaining arguments, or an empty list if the method was called with a single argument.
    // 5. If connection.[[IsClosed]] is true, throw an InvalidStateError.
    if (m_is_closed)
        return WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16);
    // 6. Let senders be the result of executing the CollectSenders algorithm. If an RTCRtpSender for track already exists in senders, throw an InvalidAccessError.
    auto senders = collect_senders();
    for (auto const& existing : senders) {
        if (existing->track() == track)
            return WebIDL::InvalidAccessError::create("RTCRtpSender for track already exists"_utf16);
    }
    // 7. ...if any RTCRtpSender object in senders matches all the following criteria, let sender be that object, or null otherwise:
    //    - The sender's track is null.
    //    - The transceiver kind of the RTCRtpTransceiver, associated with the sender, matches kind.
    //    - The [[Stopping]] slot of the RTCRtpTransceiver associated with the sender is false.
    //    - The sender has never been used to send. More precisely, the [[CurrentDirection]] slot of the RTCRtpTransceiver associated with the sender has never had a value of "sendrecv" or "sendonly".
    GC::Ptr<RTCRtpSender> sender;
    GC::Ptr<RTCRtpTransceiver> matching_transceiver;
    for (auto& transceiver : m_transceivers) {
        if (transceiver->sender()->track() != nullptr)
            continue;
        if (transceiver->kind() != kind)
            continue;
        if (transceiver->is_stopping())
            continue;
        // FIXME: track [[CurrentDirection]] history; for now we approximate by checking the current value.
        auto current = transceiver->current_direction();
        if (current.has_value() && (*current == Bindings::RTCRtpTransceiverDirection::Sendrecv || *current == Bindings::RTCRtpTransceiverDirection::Sendonly))
            continue;
        sender = transceiver->sender();
        matching_transceiver = transceiver;
        break;
    }
    // 8. If sender is not null, run the following steps to use that sender:
    if (sender) {
        // 8.1. Set sender.[[SenderTrack]] to track.
        sender->set_track(track);
        // 8.2. Set sender.[[AssociatedMediaStreamIds]] to an empty set.
        Vector<Utf16String> ids;
        // 8.3. For each stream in streams, add stream.id to [[AssociatedMediaStreamIds]] if it's not already there.
        for (auto const& stream : streams) {
            auto id = stream->id();
            if (!ids.contains_slow(id))
                ids.append(move(id));
        }
        sender->set_associated_media_stream_ids(move(ids));
        // 8.4. Let transceiver be the RTCRtpTransceiver associated with sender.
        // 8.5. If transceiver.[[Direction]] is "recvonly", set transceiver.[[Direction]] to "sendrecv".
        if (matching_transceiver->direction() == Bindings::RTCRtpTransceiverDirection::Recvonly)
            matching_transceiver->set_direction(Bindings::RTCRtpTransceiverDirection::Sendrecv);
        // 8.6. If transceiver.[[Direction]] is "inactive", set transceiver.[[Direction]] to "sendonly".
        else if (matching_transceiver->direction() == Bindings::RTCRtpTransceiverDirection::Inactive)
            matching_transceiver->set_direction(Bindings::RTCRtpTransceiverDirection::Sendonly);
    }
    auto& agent = WebRTCAgent::the();
    // 9. If sender is null, run the following steps:
    if (!sender) {
        auto sender_id = agent.next_sender_id();
        auto sender_ssrc = agent.next_ssrc();
        // FIXME: 9.1. Create an RTCRtpSender with track, kind and streams, and let sender be the result.
        sender = RTCRtpSender::create(*this, sender_id, sender_ssrc);
        sender->set_track(track);
        Vector<Utf16String> ids;
        for (auto const& stream : streams) {
            auto id = stream->id();
            if (!ids.contains_slow(id))
                ids.append(move(id));
        }
        sender->set_associated_media_stream_ids(move(ids));
        // 9.2. Create an RTCRtpReceiver with kind, and let receiver be the result.
        auto receiver = RTCRtpReceiver::create(kind);
        // FIXME: 9.3. Create an RTCRtpTransceiver with sender, receiver and an RTCRtpTransceiverDirection value of "sendrecv", and let transceiver be the result.
        auto transceiver = RTCRtpTransceiver::create(*this, *sender, receiver, Bindings::RTCRtpTransceiverDirection::Sendrecv, kind);
        transceiver->set_transceiver_id(sender_id);
        // 9.4. Add transceiver to connection's set of transceivers.
        m_transceivers.append(transceiver);
    }
    // FIXME: 10. A track could have contents that are inaccessible to the application. ... Silence (audio), black frames (video) or equivalently absent content is sent in place of track content.
    // 11. Update the negotiation-needed flag for connection.
    update_negotiation_needed_flag();
    // 12. Return sender.
    return GC::Ref { *sender };
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-addtransceiver
WebIDL::ExceptionOr<GC::Ref<RTCRtpTransceiver>> RTCPeerConnection::add_transceiver(Variant<GC::Ref<MediaCapture::MediaStreamTrack>, Utf16String> const& track_or_kind, RTCRtpTransceiverInit const& init)
{
    // 1. Let connection be the RTCPeerConnection object on which the method was invoked.
    // 2. If connection.[[IsClosed]] is true, throw an InvalidStateError.
    if (m_is_closed)
        return WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16);
    // 3. Let init be the method's second argument.
    // 4. Let trackOrKind be the method's first argument.
    // 5. If trackOrKind is a kind, then let kind be trackOrKind, and let track be null.
    //    Otherwise, let kind be trackOrKind.kind and let track be trackOrKind.
    GC::Ptr<MediaCapture::MediaStreamTrack> track;
    Bindings::MediaStreamTrackKind kind;
    if (track_or_kind.has<Utf16String>()) {
        auto const& kind_string = track_or_kind.get<Utf16String>();
        // 6. If kind is not equal to "audio" or "video", throw a TypeError.
        if (kind_string == "audio"sv) {
            kind = Bindings::MediaStreamTrackKind::Audio;
        } else if (kind_string == "video"sv) {
            kind = Bindings::MediaStreamTrackKind::Video;
        } else {
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "addTransceiver: kind must be \"audio\" or \"video\""_utf16 };
        }
    } else {
        track = *track_or_kind.get<GC::Ref<MediaCapture::MediaStreamTrack>>();
        kind = track->track_kind();
    }
    // FIXME: 7. Verify that each value in init.sendEncodings conforms to the set of "RTCRtpEncodingParameters dictionary" requirements provided in the send encodings parameters section. If one of the values does not meet these requirements, throw a RangeError.
    // FIXME: 8. ...remaining sendEncodings normalization steps from the spec.
    // 9. Let sendEncodings be the value of init.sendEncodings.
    (void)init.send_encodings;
    // 10. Let direction be the value of init.direction.
    auto direction = init.direction;
    // 11. Create an RTCRtpSender, sender, from track, kind, init.streams, and sendEncodings.
    auto sender_id_for_new_sender = WebRTCAgent::the().next_sender_id();
    auto sender_ssrc_for_new_sender = WebRTCAgent::the().next_ssrc();
    auto sender = RTCRtpSender::create(*this, sender_id_for_new_sender, sender_ssrc_for_new_sender);
    Vector<Utf16String> stream_ids;
    for (auto const& stream : init.streams) {
        auto id = stream->id();
        if (!stream_ids.contains_slow(id))
            stream_ids.append(move(id));
    }
    sender->set_associated_media_stream_ids(move(stream_ids));
    // 12. Create an RTCRtpReceiver, receiver, from kind.
    auto receiver = RTCRtpReceiver::create(kind);
    // 13. Create an RTCRtpTransceiver, transceiver, from sender, receiver, and direction.
    auto transceiver = RTCRtpTransceiver::create(*this, sender, receiver, direction, kind);
    // 14. Add transceiver to connection's set of transceivers.
    m_transceivers.append(transceiver);
    auto& agent = WebRTCAgent::the();
    auto transceiver_id = sender_id_for_new_sender;
    transceiver->set_transceiver_id(transceiver_id);
    if (auto* client = agent.client()) {
        client->async_add_transceiver(
            m_pc_id,
            transceiver_id,
            kind == Bindings::MediaStreamTrackKind::Audio ? "audio"_string : "video"_string,
            idl_enum_to_string(direction).to_well_formed_utf8());
    }
    sender->set_track(track);
    // 15. Update the negotiation-needed flag for connection.
    update_negotiation_needed_flag();
    // 16. Return transceiver.
    return transceiver;
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-getsenders (CollectSenders)
GC::RootVector<GC::Ref<RTCRtpSender>> RTCPeerConnection::collect_senders() const
{
    // 1. Let transceivers be the result of executing the CollectTransceivers algorithm.
    auto transceivers = collect_transceivers();
    // 2. Let senders be a new empty sequence.
    GC::RootVector<GC::Ref<RTCRtpSender>> senders;
    // 3. For each transceiver in transceivers,
    for (auto& transceiver : transceivers) {
        // 3.1. If transceiver.[[Stopped]] is false, add transceiver.[[Sender]] to senders.
        if (!transceiver->is_stopped())
            senders.append(transceiver->sender());
    }
    // 4. Return senders.
    return senders;
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-createdatachannel
WebIDL::ExceptionOr<GC::Ref<RTCDataChannel>> RTCPeerConnection::create_data_channel(Utf16String const& label, RTCDataChannelInit const& options)
{
    // 1. Let connection be the RTCPeerConnection object on which the method is invoked.
    // 2. If connection.[[IsClosed]] is true, throw an InvalidStateError.
    if (m_is_closed)
        return WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16);
    // 3. Create an RTCDataChannel, channel.
    auto channel = RTCDataChannel::create(m_global_object);
    // 4. Initialize channel.[[DataChannelLabel]] to the value of the first argument.
    channel->set_label(label);
    // 5. If the UTF-8 representation of [[DataChannelLabel]] is longer than 65535 bytes, throw a TypeError.
    auto label_utf8 = label.to_well_formed_utf8();
    if (label_utf8.bytes().size() > 65535)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "RTCDataChannel label exceeds 65535 bytes"_utf16 };
    // 6. Let options be the second argument.
    // 7. Initialize channel.[[MaxPacketLifeTime]] to option.maxPacketLifeTime, if present, otherwise null.
    channel->set_max_packet_life_time(options.max_packet_life_time);
    // 8. Initialize channel.[[MaxRetransmits]] to option.maxRetransmits, if present, otherwise null.
    channel->set_max_retransmits(options.max_retransmits);
    // 9. Initialize channel.[[Ordered]] to option.ordered.
    channel->set_ordered(options.ordered);
    // 10. Initialize channel.[[DataChannelProtocol]] to option.protocol.
    channel->set_protocol(options.protocol);
    // 11. If the UTF-8 representation of [[DataChannelProtocol]] is longer than 65535 bytes, throw a TypeError.
    auto protocol_utf8 = options.protocol.to_well_formed_utf8();
    if (protocol_utf8.bytes().size() > 65535)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "RTCDataChannel protocol exceeds 65535 bytes"_utf16 };
    // 12. Initialize channel.[[Negotiated]] to option.negotiated.
    channel->set_negotiated(options.negotiated);
    // 13. Initialize channel.[[DataChannelId]] to the value of option.id, if it is present and [[Negotiated]] is true, otherwise null.
    if (options.id.has_value() && options.negotiated)
        channel->set_id(options.id);
    else
        channel->set_id({});
    // 14. If [[Negotiated]] is true and [[DataChannelId]] is null, throw a TypeError.
    if (options.negotiated && !options.id.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "RTCDataChannel: negotiated=true requires id"_utf16 };
    // 15. If both [[MaxPacketLifeTime]] and [[MaxRetransmits]] attributes are set (not null), throw a TypeError.
    if (options.max_packet_life_time.has_value() && options.max_retransmits.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "RTCDataChannel: maxPacketLifeTime and maxRetransmits are mutually exclusive"_utf16 };
    // FIXME: 16. If a setting, either [[MaxPacketLifeTime]] or [[MaxRetransmits]], has been set to indicate unreliable mode, and that value exceeds the maximum value supported by the user agent, the value MUST be set to the user agents maximum value.
    // 17. If [[DataChannelId]] is equal to 65535, which is greater than the maximum allowed ID of 65534 but still qualifies as an unsigned short, throw a TypeError.
    if (options.id.has_value() && *options.id == 65535)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "RTCDataChannel id 65535 is reserved"_utf16 };
    // FIXME: 18. If the [[DataChannelId]] slot is null (due to no ID being passed into createDataChannel, or [[Negotiated]] being false), and the DTLS role of the SCTP transport has already been negotiated, then initialize [[DataChannelId]] to a value generated by the user agent, according to [RFC8832], and skip to the next step. If no available ID could be generated, or if the value of the [[DataChannelId]] slot is being used by an existing RTCDataChannel, throw an OperationError exception.
    // FIXME: 19. Let transport be connection.[[SctpTransport]]. If the [[DataChannelId]] slot is not null, transport is in the "connected" state and [[DataChannelId]] is greater or equal to transport.[[MaxChannels]], throw an OperationError.
    // 20. If channel is the first RTCDataChannel created on connection, update the negotiation-needed flag for connection.
    if (m_data_channels.is_empty())
        update_negotiation_needed_flag();
    // 21. Append channel to connection.[[DataChannels]].
    m_data_channels.append(channel);
    // 22. Return channel and continue the following steps in parallel.
    // 23. Create channel's associated underlying data transport and configure it according to the relevant properties of channel.
    auto& agent = WebRTCAgent::the();
    auto channel_id = agent.next_channel_id();
    channel->set_channel_id(channel_id);
    m_data_channels_by_id.set(channel_id, channel);
    agent.register_data_channel(channel_id, channel);
    if (auto* client = agent.client()) {
        client->async_add_data_channel(
            m_pc_id,
            channel_id,
            label_utf8,
            options.ordered,
            options.max_packet_life_time,
            options.max_retransmits,
            protocol_utf8,
            options.negotiated,
            options.id);
    }
    return channel;
}

// https://www.w3.org/TR/webrtc/#collect-transceivers
GC::RootVector<GC::Ref<RTCRtpTransceiver>> RTCPeerConnection::collect_transceivers() const
{
    // 1. Let transceivers be a new sequence consisting of all RTCRtpTransceiver objects in this RTCPeerConnection object's set of transceivers, in insertion order.
    // 2. Return transceivers.
    return GC::RootVector { m_transceivers.span() };
}

// FIXME: https://www.w3.org/TR/webrtc/#update-the-negotiation-needed-flag
void RTCPeerConnection::restart_ice()
{
    if (m_is_closed)
        return;
    m_restart_ice = true;
    update_negotiation_needed_flag();
}

void RTCPeerConnection::update_negotiation_needed_flag()
{
    HTML::queue_a_task(HTML::Task::Source::Networking, nullptr, nullptr, GC::create_function(heap(), [this] {
        if (!m_is_closed && m_signaling_state == Bindings::RTCSignalingState::Stable)
            on_negotiation_needed_received();
    }));
}

// https://www.w3.org/TR/webrtc/#dom-rtcpeerconnection-getstats
GC::Ref<WebIDL::Promise> RTCPeerConnection::get_stats(GC::Ptr<MediaCapture::MediaStreamTrack> selector)
{
    auto promise = WebIDL::create_promise(relevant_realm());
    if (selector) {
        // The backend currently returns connection-wide reports. Do not silently
        // return unrelated statistics when a caller requests a particular track.
        WebIDL::reject_promise(relevant_realm(), promise, WebIDL::NotSupportedError::create("Track-scoped RTC statistics are not supported"_utf16));
        return promise;
    }
    auto* client = WebRTCAgent::the().existing_client();
    if (!client || m_is_closed) {
        auto report = RTCStatsReport::create();
        WebIDL::resolve_promise(relevant_realm(), promise, Bindings::wrap(Bindings::host_defined_wrapper_world(relevant_realm()), relevant_realm(), report));
        return promise;
    }
    auto request_id = WebRTCAgent::the().next_request_id();
    m_pending_description_requests.set(request_id, promise);
    client->async_get_stats(m_pc_id, request_id);
    return promise;
}

void RTCPeerConnection::on_stats_received(u64 request_id, String reports, String error)
{
    auto pending = m_pending_description_requests.take(request_id);
    if (!pending.has_value())
        return;
    auto& realm = relevant_realm();
    HTML::TemporaryExecutionContext context(realm);
    if (!error.is_empty()) {
        WebIDL::reject_promise(realm, **pending, WebIDL::OperationError::create(Utf16String::from_utf8(error)));
        return;
    }
    auto parsed = Infra::parse_json_string_to_javascript_value(realm, Utf16String::from_utf8(reports));
    if (parsed.is_exception() || !parsed.value().is_object()) {
        WebIDL::reject_promise(realm, **pending, WebIDL::OperationError::create("Invalid RTC statistics report"_utf16));
        return;
    }
    auto& object = parsed.value().as_object();
    auto report = RTCStatsReport::create();
    for (auto key : MUST(object.enumerable_own_property_names(JS::Object::PropertyKind::Key))) {
        auto name = MUST(key.to_utf16_string(realm.vm()));
        auto entry = MUST(object.get(name));
        if (entry.is_object())
            report->set_entry(move(name), entry.as_object());
    }
    WebIDL::resolve_promise(realm, **pending, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, report));
}

static Optional<Bindings::RTCSignalingState> parse_signaling_state(StringView s)
{
    if (s == "stable"sv)
        return Bindings::RTCSignalingState::Stable;
    if (s == "have-local-offer"sv || s == "havelocaloffer"sv)
        return Bindings::RTCSignalingState::HaveLocalOffer;
    if (s == "have-remote-offer"sv || s == "haveremoteoffer"sv)
        return Bindings::RTCSignalingState::HaveRemoteOffer;
    if (s == "have-local-pranswer"sv || s == "havelocalpranswer"sv)
        return Bindings::RTCSignalingState::HaveLocalPranswer;
    if (s == "have-remote-pranswer"sv || s == "haveremotepranswer"sv)
        return Bindings::RTCSignalingState::HaveRemotePranswer;
    if (s == "closed"sv)
        return Bindings::RTCSignalingState::Closed;
    return {};
}

static Optional<Bindings::RTCPeerConnectionState> parse_connection_state(StringView s)
{
    if (s == "new"sv)
        return Bindings::RTCPeerConnectionState::New;
    if (s == "connecting"sv)
        return Bindings::RTCPeerConnectionState::Connecting;
    if (s == "connected"sv)
        return Bindings::RTCPeerConnectionState::Connected;
    if (s == "disconnected"sv)
        return Bindings::RTCPeerConnectionState::Disconnected;
    if (s == "failed"sv)
        return Bindings::RTCPeerConnectionState::Failed;
    if (s == "closed"sv)
        return Bindings::RTCPeerConnectionState::Closed;
    return {};
}

static Optional<Bindings::RTCIceGatheringState> parse_ice_gathering(StringView s)
{
    if (s == "new"sv)
        return Bindings::RTCIceGatheringState::New;
    if (s == "gathering"sv)
        return Bindings::RTCIceGatheringState::Gathering;
    if (s == "complete"sv)
        return Bindings::RTCIceGatheringState::Complete;
    return {};
}

static Optional<Bindings::RTCIceConnectionState> parse_ice_connection(StringView s)
{
    if (s == "new"sv)
        return Bindings::RTCIceConnectionState::New;
    if (s == "checking"sv)
        return Bindings::RTCIceConnectionState::Checking;
    if (s == "connected"sv)
        return Bindings::RTCIceConnectionState::Connected;
    if (s == "completed"sv)
        return Bindings::RTCIceConnectionState::Completed;
    if (s == "disconnected"sv)
        return Bindings::RTCIceConnectionState::Disconnected;
    if (s == "failed"sv)
        return Bindings::RTCIceConnectionState::Failed;
    if (s == "closed"sv)
        return Bindings::RTCIceConnectionState::Closed;
    return {};
}

void RTCPeerConnection::on_signaling_state_event(String state)
{
    if (m_is_closed)
        return;
    if (auto parsed = parse_signaling_state(state); parsed.has_value()) {
        m_signaling_state = *parsed;
        HTML::TemporaryExecutionContext context(relevant_realm());
        dispatch_event(DOM::Event::create(relevant_global_object(), HTML::EventNames::signalingstatechange));
    }
}

void RTCPeerConnection::on_connection_state_event(String state)
{
    if (m_is_closed)
        return;
    if (auto parsed = parse_connection_state(state); parsed.has_value()) {
        m_connection_state = *parsed;
        HTML::TemporaryExecutionContext context(relevant_realm());
        dispatch_event(DOM::Event::create(relevant_global_object(), HTML::EventNames::connectionstatechange));
    }
}

void RTCPeerConnection::on_ice_gathering_state_event(String state)
{
    if (m_is_closed)
        return;
    // Gathering starts as the local description is set; see on_ice_candidate_received().
    if (is_setting_local_description()) {
        m_held_ice_events.append([this, state = move(state)]() mutable { on_ice_gathering_state_event(move(state)); });
        return;
    }
    if (auto parsed = parse_ice_gathering(state); parsed.has_value()) {
        m_ice_gathering_state = *parsed;
        HTML::TemporaryExecutionContext context(relevant_realm());
        dispatch_event(DOM::Event::create(relevant_global_object(), HTML::EventNames::icegatheringstatechange));
    }
}

void RTCPeerConnection::on_ice_connection_state_event(String state)
{
    if (m_is_closed)
        return;
    if (auto parsed = parse_ice_connection(state); parsed.has_value()) {
        m_ice_connection_state = *parsed;
        HTML::TemporaryExecutionContext context(relevant_realm());
        dispatch_event(DOM::Event::create(relevant_global_object(), HTML::EventNames::iceconnectionstatechange));
    }
}

void RTCPeerConnection::on_ice_candidate_received(Optional<String> candidate, Optional<String> sdp_mid, Optional<u32> sdp_mline_index)
{
    if (m_is_closed)
        return;
    // webrtc-rs gathers while it sets the local description, so its candidates can reach us before the result. The
    // page must only see them once setLocalDescription() has resolved, or it may have nowhere to send them yet: a
    // peer without a remote description rejects them.
    // https://w3c.github.io/webrtc-pc/#dom-peerconnection-setlocaldescription
    if (is_setting_local_description()) {
        m_held_ice_events.append([this, candidate = move(candidate), sdp_mid = move(sdp_mid), sdp_mline_index]() mutable {
            on_ice_candidate_received(move(candidate), move(sdp_mid), sdp_mline_index);
        });
        return;
    }
    HTML::TemporaryExecutionContext context(relevant_realm());
    RTCPeerConnectionIceEventInit init;
    if (candidate.has_value()) {
        RTCIceCandidateInit candidate_init;
        candidate_init.candidate = Utf16String::from_utf8(*candidate);
        if (sdp_mid.has_value())
            candidate_init.sdp_mid = Utf16String::from_utf8(*sdp_mid);
        if (sdp_mline_index.has_value())
            candidate_init.sdp_m_line_index = static_cast<u16>(*sdp_mline_index);
        init.candidate = RTCIceCandidate::create(candidate_init);
    }
    dispatch_event(RTCPeerConnectionIceEvent::create(HTML::EventNames::icecandidate, init, HighResolutionTime::current_high_resolution_time(relevant_global_object())));
}

void RTCPeerConnection::on_ice_candidate_error_received(Optional<String>, Optional<u16>, String, u16, String)
{
    // FIXME: dispatch an RTCPeerConnectionIceErrorEvent with the right fields.
    HTML::TemporaryExecutionContext context(relevant_realm());
    dispatch_event(DOM::Event::create(relevant_global_object(), HTML::EventNames::icecandidateerror));
}

void RTCPeerConnection::on_negotiation_needed_received()
{
    if (m_is_closed)
        return;
    HTML::TemporaryExecutionContext context(relevant_realm());
    dispatch_event(DOM::Event::create(relevant_global_object(), HTML::EventNames::negotiationneeded));
}

namespace {

struct TrackEventInit {
    GC::Ref<RTCRtpReceiver> receiver;
    GC::Ref<MediaCapture::MediaStreamTrack> track;
    GC::RootVector<GC::Ref<MediaCapture::MediaStream>> streams;
    GC::Ref<RTCRtpTransceiver> transceiver;
};

// Spec uses "(stream, track) pairs" in addList/removeList; alias for clarity.
struct StreamTrackPair {
    GC::Ref<MediaCapture::MediaStream> stream;
    GC::Ref<MediaCapture::MediaStreamTrack> track;
};

constexpr bool direction_is_receive(Bindings::RTCRtpTransceiverDirection d)
{
    return d == Bindings::RTCRtpTransceiverDirection::Sendrecv
        || d == Bindings::RTCRtpTransceiverDirection::Recvonly;
}

constexpr bool direction_is_send_only_or_inactive(Bindings::RTCRtpTransceiverDirection d)
{
    return d == Bindings::RTCRtpTransceiverDirection::Sendonly
        || d == Bindings::RTCRtpTransceiverDirection::Inactive;
}

}

// https://www.w3.org/TR/webrtc/#set-associated-remote-streams
static void set_the_associated_remote_streams(RTCPeerConnection& pc, RTCRtpReceiver& receiver,
    Vector<String> const& msids,
    GC::ConservativeVector<StreamTrackPair>& add_list,
    GC::ConservativeVector<StreamTrackPair>& remove_list)
{
    // 1. Let connection be the RTCPeerConnection object associated with receiver.
    auto& connection = pc;

    // 2. For each MSID in msids, unless a MediaStream object has previously been
    //    created with that id for this connection, create a MediaStream object with
    //    that id.
    // 3. Let streams be a list of the MediaStream objects created for this connection
    //    with the ids corresponding to msids.
    GC::RootVector<GC::Ref<MediaCapture::MediaStream>> streams;
    streams.ensure_capacity(msids.size());
    for (auto const& msid : msids) {
        if (auto stream = connection.find_or_create_remote_stream_for_msid(msid))
            streams.append(*stream);
    }

    // 4. Let track be receiver.[[ReceiverTrack]].
    auto track = receiver.track();

    // 5. For each stream in receiver.[[AssociatedRemoteMediaStreams]] that is not
    //    present in streams, add stream and track as a pair to removeList.
    for (auto const& previous : receiver.associated_remote_streams()) {
        bool still_associated = false;
        for (auto const& fresh : streams) {
            if (fresh.ptr() == previous.ptr()) {
                still_associated = true;
                break;
            }
        }
        if (!still_associated)
            remove_list.append({ previous, track });
    }

    // 6. For each stream in streams that is not present in
    //    receiver.[[AssociatedRemoteMediaStreams]], add stream and track as a pair
    //    to addList.
    for (auto const& fresh : streams) {
        bool was_already_associated = false;
        for (auto const& previous : receiver.associated_remote_streams()) {
            if (previous.ptr() == fresh.ptr()) {
                was_already_associated = true;
                break;
            }
        }
        if (!was_already_associated)
            add_list.append({ fresh, track });
    }

    // 7. Set receiver.[[AssociatedRemoteMediaStreams]] to streams.
    receiver.set_associated_remote_streams(move(streams));
}

// https://www.w3.org/TR/webrtc/#process-the-addition-of-a-remote-track
static void process_addition_of_remote_track(RTCRtpTransceiver& transceiver, GC::ConservativeVector<TrackEventInit>& track_event_inits)
{
    // 1. Let receiver be transceiver.[[Receiver]].
    auto receiver = transceiver.receiver();
    // 2. Let track be receiver.[[ReceiverTrack]].
    auto track = receiver->track();
    // 3. Let streams be receiver.[[AssociatedRemoteMediaStreams]].
    GC::RootVector streams { receiver->associated_remote_streams().span() };
    // 4. Create a new RTCTrackEventInit dictionary with receiver, track, streams and
    //    transceiver as members and add it to trackEventInits.
    track_event_inits.append({ receiver, track, move(streams), GC::Ref { transceiver } });
}

// https://www.w3.org/TR/webrtc/#process-the-removal-of-a-remote-track
static void process_removal_of_remote_track(RTCRtpTransceiver& transceiver, GC::RootVector<GC::Ref<MediaCapture::MediaStreamTrack>>& mute_tracks)
{
    // 1. Let receiver be transceiver.[[Receiver]].
    auto receiver = transceiver.receiver();
    // 2. Let track be receiver.[[ReceiverTrack]].
    auto track = receiver->track();
    // 3. If track.muted is false, add track to muteTracks.
    if (!track->muted())
        mute_tracks.append(track);
}

// https://www.w3.org/TR/webrtc/#process-remote-tracks
static void process_remote_tracks(RTCPeerConnection& pc, RTCRtpTransceiver& transceiver,
    Bindings::RTCRtpTransceiverDirection direction, Vector<String> const& msids,
    GC::ConservativeVector<StreamTrackPair>& add_list,
    GC::ConservativeVector<StreamTrackPair>& remove_list,
    GC::ConservativeVector<TrackEventInit>& track_event_inits,
    GC::RootVector<GC::Ref<MediaCapture::MediaStreamTrack>>& mute_tracks)
{
    auto add_list_size_before = add_list.size();
    // 1. Set the associated remote streams with transceiver.[[Receiver]], msids,
    //    addList, and removeList.
    set_the_associated_remote_streams(pc, transceiver.receiver(), msids, add_list, remove_list);

    auto fired = transceiver.fired_direction();
    bool fired_was_recv = fired.has_value() && direction_is_receive(fired.value());
    bool add_list_grew = add_list.size() > add_list_size_before;
    // 2. If direction is "sendrecv" or "recvonly" and transceiver.[[FiredDirection]]
    //    is neither "sendrecv" nor "recvonly", or the previous step increased the
    //    length of addList, process the addition of a remote track with transceiver
    //    and trackEventInits.
    if ((direction_is_receive(direction) && !fired_was_recv) || add_list_grew)
        process_addition_of_remote_track(transceiver, track_event_inits);

    // 3. If direction is "sendonly" or "inactive", set transceiver.[[Receptive]] to false.
    if (direction_is_send_only_or_inactive(direction))
        transceiver.set_receptive(false);

    // 4. If direction is "sendonly" or "inactive", and transceiver.[[FiredDirection]]
    //    is either "sendrecv" or "recvonly", process the removal of a remote track
    //    for the media description, with transceiver and muteTracks.
    if (direction_is_send_only_or_inactive(direction) && fired_was_recv)
        process_removal_of_remote_track(transceiver, mute_tracks);

    // 5. Set transceiver.[[FiredDirection]] to direction.
    transceiver.set_fired_direction(direction);
}

void RTCPeerConnection::on_remote_track_added(u64 receiver_id, u64 transceiver_id, String kind_string, Vector<String> stream_ids)
{
    auto& realm = relevant_realm();
    HTML::TemporaryExecutionContext context(realm);

    auto kind = kind_string == "video"sv ? Bindings::MediaStreamTrackKind::Video : Bindings::MediaStreamTrackKind::Audio;

    // The backend tells us which of our transceivers the track arrived on when we created it.
    // Otherwise, match a transceiver of matching kind whose [[FiredDirection]] hasn't already
    // been set to a receive direction, and synthesize one if the remote surprised us with a
    // track we never asked for.
    // (Spec: this matching is normally done by "apply a remote description"; we get a
    //  pre-matched track via IPC, so we approximate the same selection here.)
    GC::Ptr<RTCRtpTransceiver> transceiver;
    if (transceiver_id != 0) {
        for (auto& t : m_transceivers) {
            if (t->transceiver_id() == transceiver_id && t->kind() == kind && !t->is_stopped()) {
                transceiver = t;
                break;
            }
        }
    }
    for (auto& t : m_transceivers) {
        if (transceiver)
            break;
        if (t->kind() != kind || t->is_stopped())
            continue;
        if (t->fired_direction().has_value() && direction_is_receive(t->fired_direction().value()))
            continue;
        transceiver = t;
    }
    if (!transceiver) {
        auto synth_sender_id = WebRTCAgent::the().next_sender_id();
        auto synth_sender_ssrc = WebRTCAgent::the().next_ssrc();
        auto sender = RTCRtpSender::create(*this, synth_sender_id, synth_sender_ssrc);
        auto receiver = RTCRtpReceiver::create(kind);
        auto new_transceiver = RTCRtpTransceiver::create(*this, sender, receiver, Bindings::RTCRtpTransceiverDirection::Recvonly, kind);
        m_transceivers.append(new_transceiver);
        transceiver = new_transceiver;
    }

    auto receiver = transceiver->receiver();
    m_remote_receivers_by_id.set(receiver_id, receiver);

    // Drive the spec's "process remote tracks" algorithm. The IPC only signals new
    // tracks today (no removals), so direction is recvonly.
    GC::ConservativeVector<StreamTrackPair> add_list;
    GC::ConservativeVector<StreamTrackPair> remove_list;
    GC::ConservativeVector<TrackEventInit> track_event_inits;
    GC::RootVector<GC::Ref<MediaCapture::MediaStreamTrack>> mute_tracks;
    process_remote_tracks(*this, *transceiver, Bindings::RTCRtpTransceiverDirection::Recvonly,
        stream_ids, add_list, remove_list, track_event_inits, mute_tracks);

    // The apply-remote-description algorithm follows up by:
    //   - Adding each (stream, track) pair from addList to its stream's track set
    //     (which fires "addtrack" on the stream).
    //   - Removing each (stream, track) pair from removeList from its stream's track
    //     set (which fires "removetrack" on the stream).
    //   - Firing one RTCTrackEvent per init.
    //   - Muting each track in muteTracks.
    for (auto& pair : add_list)
        pair.stream->add_track(pair.track);
    for (auto& pair : remove_list)
        pair.stream->remove_track(pair.track);
    for (auto& init : track_event_inits) {
        auto event = RTCTrackEvent::create(HTML::EventNames::track,
            init.receiver, init.track, move(init.streams), init.transceiver,
            HighResolutionTime::current_high_resolution_time(relevant_global_object()));
        dispatch_event(event);
    }
    // FIXME: muteTracks step is "set track.muted to true and fire mute on it";
    //        MediaStreamTrack.muted is a getter so we'd need an internal set_muted.
    (void)mute_tracks;
}

GC::Ptr<MediaCapture::MediaStream> RTCPeerConnection::find_or_create_remote_stream_for_msid(String const& msid)
{
    if (auto existing = m_remote_streams.get(msid); existing.has_value())
        return existing.value();
    auto fresh = MediaCapture::MediaStream::create_with_id(Utf16String::from_utf8(msid));
    m_remote_streams.set(msid, fresh);
    return fresh;
}

void RTCPeerConnection::stop_sender(RTCRtpSender& sender)
{
    auto id = sender.sender_id();
    auto pipeline = m_outgoing_audio_pipelines.take(id);
    if (pipeline.has_value() && (*pipeline)->track && (*pipeline)->sink)
        (*pipeline)->track->remove_audio_sink(*(*pipeline)->sink);
    if (m_requested_audio_senders.contains(id)) {
        if (auto* client = WebRTCAgent::the().existing_client())
            client->async_remove_track(m_pc_id, id);
    }
    m_requested_audio_senders.remove(id);
    m_ready_audio_senders.remove(id);
}

WebIDL::ExceptionOr<void> RTCPeerConnection::remove_track(GC::Ref<RTCRtpSender> sender)
{
    if (m_is_closed)
        return WebIDL::InvalidStateError::create("RTCPeerConnection is closed"_utf16);
    if (sender->connection().ptr() != this)
        return WebIDL::InvalidAccessError::create("Sender belongs to another connection"_utf16);
    if (!sender->track())
        return {};
    stop_sender(*sender);
    sender->set_track(nullptr);
    for (auto& transceiver : m_transceivers) {
        if (transceiver->sender() != sender)
            continue;
        if (transceiver->direction() == Bindings::RTCRtpTransceiverDirection::Sendrecv)
            transceiver->set_direction(Bindings::RTCRtpTransceiverDirection::Recvonly);
        else if (transceiver->direction() == Bindings::RTCRtpTransceiverDirection::Sendonly)
            transceiver->set_direction(Bindings::RTCRtpTransceiverDirection::Inactive);
    }
    update_negotiation_needed_flag();
    return {};
}

void RTCPeerConnection::on_sender_track_changed(RTCRtpSender& sender)
{
    if (m_is_closed)
        return;
    auto sender_id = sender.sender_id();
    auto old = m_outgoing_audio_pipelines.take(sender_id);
    if (old.has_value() && (*old)->track && (*old)->sink)
        (*old)->track->remove_audio_sink(*(*old)->sink);
    auto track = sender.track();
    if (!track || track->track_kind() != Bindings::MediaStreamTrackKind::Audio)
        return;
    if (m_ready_audio_senders.contains(sender_id)) {
        start_outgoing_audio_for_sender(sender);
        return;
    }
    if (m_requested_audio_senders.contains(sender_id))
        return;
    m_requested_audio_senders.set(sender_id);
    if (auto* client = WebRTCAgent::the().existing_client())
        client->async_add_audio_track(m_pc_id, sender_id);
}

void RTCPeerConnection::on_audio_track_ssrc_assigned(u64 sender_id, u32 ssrc)
{
    if (m_is_closed)
        return;
    GC::Ptr<RTCRtpSender> sender;
    for (auto& transceiver : m_transceivers) {
        if (transceiver->sender()->sender_id() == sender_id) {
            sender = transceiver->sender();
            break;
        }
    }
    if (!sender) {
        dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: on_audio_track_ssrc_assigned for unknown sender_id={}", sender_id);
        return;
    }
    if (!m_requested_audio_senders.contains(sender_id))
        return;
    sender->set_ssrc(ssrc);
    m_ready_audio_senders.set(sender_id);
    if (m_outgoing_audio_pipelines.contains(sender_id))
        return;
    start_outgoing_audio_for_sender(*sender);
}

void RTCPeerConnection::on_sender_transform_changed(RTCRtpSender& sender)
{
    auto script_transform = sender.script_transform();
    if (!script_transform)
        return;
    auto sender_id = sender.sender_id();
    if (script_transform->has_frame_written_callback())
        return;
    script_transform->set_on_frame_written([self = GC::Weak { *this }, sender_id](ByteBuffer payload, u32, u8 payload_type, u32 rtp_timestamp, u16 sequence_number) {
        if (self)
            self->on_outgoing_encrypted_frame(sender_id, move(payload), rtp_timestamp, sequence_number, payload_type);
    });
}

void RTCPeerConnection::start_outgoing_audio_for_sender(GC::Ref<RTCRtpSender> sender)
{
    auto sender_id = sender->sender_id();
    auto track = sender->track();
    if (!track) {
        dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: sender_id={} has no track yet, deferring", sender_id);
        return;
    }
    auto sample_spec = Audio::SampleSpecification(48000, Audio::ChannelMap::stereo());
    constexpr int OPUS_BITRATE_BPS = 64'000;
    auto encoder_or_err = Media::FFmpeg::FFmpegAudioEncoder::try_create(Media::CodecID::Opus, sample_spec, OPUS_BITRATE_BPS);
    if (encoder_or_err.is_error()) {
        dbgln("RTCPeerConnection: opus encoder init failed for sender_id={}: {}", sender_id, encoder_or_err.error().description());
        return;
    }
    auto converter = Media::FFmpeg::FFmpegAudioConverter::try_create();
    if (converter.is_error() || converter.value()->set_output_sample_specification(sample_spec).is_error())
        return;
    auto pipeline = make<OutgoingAudioPipeline>();
    auto producer = adopt_ref(*new OutgoingAudioProducerState);
    producer->converter = converter.release_value();
    pipeline->encoder = encoder_or_err.release_value();
    pipeline->generation = WebRTCAgent::the().next_request_id();
    pipeline->track = track;

    // Track sinks run on the producer thread; encoding and JS realm operations need the main loop.
    auto main_loop_weak = Core::EventLoop::current_weak();
    auto sink = adopt_ref(*new MediaCapture::AudioFrameSink);
    sink->on_frames = [pc_id = m_pc_id, sender_id, producer, generation = pipeline->generation, main_loop_weak](float const* samples, size_t frame_count, u8 channels, u32 sample_rate) {
        if (channels == 0 || frame_count == 0 || sample_rate == 0)
            return;
        constexpr size_t CHANNEL_COUNT = 2;
        constexpr size_t INTERLEAVED_SAMPLES = 960 * CHANNEL_COUNT;
        Media::AudioBlock input;
        auto timestamp = AK::Duration::from_microseconds(static_cast<i64>(producer->source_frames) * 1'000'000 / sample_rate);
        producer->source_frames += frame_count;
        input.initialize(Audio::SampleSpecification(sample_rate, Audio::ChannelMap::stereo()), timestamp, frame_count);
        for (size_t c = 0; c < CHANNEL_COUNT; ++c) {
            auto output = input.channel_data(c);
            for (size_t f = 0; f < frame_count; ++f)
                output[f] = samples[f * channels + (c < channels ? c : 0)];
        }
        if (producer->converter->push_block(input).is_error())
            return;
        auto& accum = producer->float_accumulator;
        while (true) {
            Media::AudioBlock converted;
            if (producer->converter->retrieve_block(converted).is_error() || converted.is_empty())
                break;
            auto left = converted.channel_data(0);
            auto right = converted.channel_data(1);
            for (size_t f = 0; f < converted.frame_count(); ++f) {
                accum.append(AK::clamp(left[f], -1.0f, 1.0f));
                accum.append(AK::clamp(right[f], -1.0f, 1.0f));
            }
        }
        while (accum.size() >= INTERLEAVED_SAMPLES) {
            auto buffer_or_err = ByteBuffer::create_uninitialized(INTERLEAVED_SAMPLES * sizeof(i16));
            if (buffer_or_err.is_error())
                break;
            auto buffer = buffer_or_err.release_value();
            auto* dst = reinterpret_cast<i16*>(buffer.data());
            for (size_t i = 0; i < INTERLEAVED_SAMPLES; ++i)
                dst[i] = static_cast<i16>(accum[i] * 32767.0f);
            accum.remove(0, INTERLEAVED_SAMPLES);

            auto strong = main_loop_weak->take();
            if (!strong || producer->pending_frames.load() >= 5)
                continue;
            producer->pending_frames.fetch_add(1);
            strong->deferred_invoke([pc_id, sender_id, producer, captured_at = MonotonicTime::now(), generation, buffer = move(buffer)]() mutable {
                producer->pending_frames.fetch_sub(1);
                auto self = WebRTCAgent::the().find_peer_connection(pc_id);
                if (!self || self->m_is_closed || (MonotonicTime::now() - captured_at).to_milliseconds() > 200)
                    return;
                auto pipeline = self->m_outgoing_audio_pipelines.find(sender_id);
                if (pipeline == self->m_outgoing_audio_pipelines.end() || pipeline->value->generation != generation)
                    return;
                self->encode_and_route_outgoing_pcm(sender_id, move(buffer));
            });
        }
    };
    track->add_audio_sink(sink);
    pipeline->sink = sink;
    m_outgoing_audio_pipelines.set(sender_id, move(pipeline));
}

void RTCPeerConnection::encode_and_route_outgoing_pcm(u64 sender_id, ByteBuffer pcm_s16le)
{
    if (m_is_closed)
        return;
    auto pipeline_iter = m_outgoing_audio_pipelines.find(sender_id);
    if (pipeline_iter == m_outgoing_audio_pipelines.end())
        return;
    auto& pipeline = *pipeline_iter->value;
    if (!pipeline.encoder)
        return;

    constexpr u32 SAMPLE_RATE = 48000;
    constexpr size_t CHANNEL_COUNT = 2;
    constexpr size_t SAMPLES_PER_FRAME = 960; // 20 ms
    constexpr size_t INTERLEAVED_SAMPLES = SAMPLES_PER_FRAME * CHANNEL_COUNT;

    auto sample_count = pcm_s16le.size() / sizeof(i16);
    if (sample_count != INTERLEAVED_SAMPLES) {
        dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: outgoing PCM frame has {} samples, expected {}", sample_count, INTERLEAVED_SAMPLES);
        return;
    }

    Media::AudioBlock block;
    auto timestamp = AK::Duration::from_microseconds(static_cast<i64>(pipeline.next_rtp_timestamp) * 1'000'000 / SAMPLE_RATE);
    auto sample_spec = Audio::SampleSpecification(SAMPLE_RATE, Audio::ChannelMap::stereo());
    block.initialize(sample_spec, timestamp, SAMPLES_PER_FRAME);
    auto const* samples = reinterpret_cast<i16 const*>(pcm_s16le.data());
    for (size_t channel = 0; channel < CHANNEL_COUNT; ++channel) {
        auto channel_data = block.channel_data(channel);
        for (size_t frame = 0; frame < SAMPLES_PER_FRAME; ++frame)
            channel_data[frame] = static_cast<float>(samples[(frame * CHANNEL_COUNT) + channel]) * (1.0f / 32768.0f);
    }

    if (auto rc = pipeline.encoder->receive_pcm_data(timestamp, block); rc.is_error()) {
        dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: opus receive_pcm_data failed: {}", rc.error().description());
        return;
    }
    while (true) {
        Media::FFmpeg::FFmpegAudioEncoder::Packet packet;
        auto got_packet_or_err = pipeline.encoder->write_next_packet(packet);
        if (got_packet_or_err.is_error()) {
            dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: opus write_next_packet failed: {}", got_packet_or_err.error().description());
            return;
        }
        if (!got_packet_or_err.value())
            break;

        // The sender may have been collected since capture started.
        GC::Ptr<RTCRtpSender> sender;
        for (auto& t : m_transceivers) {
            if (t->sender()->sender_id() == sender_id) {
                sender = t->sender();
                break;
            }
        }
        if (!sender)
            return;

        auto rtp_ts = pipeline.next_rtp_timestamp;
        auto seq = pipeline.next_sequence_number++;
        pipeline.next_rtp_timestamp += SAMPLES_PER_FRAME;

        auto script_transform = sender->script_transform();
        if (!script_transform) {
            if (sender->transform().has<Empty>())
                on_outgoing_encrypted_frame(sender_id, move(packet.data), rtp_ts, seq, 111);
            continue;
        }

        script_transform->enqueue_encoded_audio_frame(move(packet.data), sender->ssrc(), 111 /* opus */, rtp_ts, seq);
    }
}

void RTCPeerConnection::on_outgoing_encrypted_frame(u64 sender_id, ByteBuffer payload, u32 rtp_timestamp, u16 sequence_number, u8 payload_type)
{
    if (m_is_closed)
        return;
    (void)rtp_timestamp;
    (void)sequence_number;
    (void)payload_type;
    auto* client = WebRTCAgent::the().client();
    if (!client)
        return;
    constexpr u32 FRAME_DURATION_MICROS = 20'000;
    dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: shipping encoded frame to rust sender_id={} len={}", sender_id, payload.size());
    client->async_audio_track_encoded_frame(sender_id, FRAME_DURATION_MICROS, payload.bytes());
}

void RTCPeerConnection::on_remote_track_ended(u64 receiver_id)
{
    if (m_is_closed)
        return;
    HTML::TemporaryExecutionContext context(relevant_realm());
    if (auto receiver = m_remote_receivers_by_id.get(receiver_id); receiver.has_value())
        (*receiver)->track()->end();
    auto playback = m_receiver_audio_playbacks.take(receiver_id);
    if (playback.has_value() && (*playback)->playback_stream)
        (void)(*playback)->playback_stream->discard_buffer_and_suspend();
}

void RTCPeerConnection::on_encoded_audio_frame_received(u64 receiver_id, u32 ssrc, u32 rtp_timestamp, u16 sequence_number, u8 payload_type, ByteBuffer payload)
{
    auto receiver_iter = m_remote_receivers_by_id.get(receiver_id);
    if (!receiver_iter.has_value())
        return;
    auto receiver = *receiver_iter;

    auto transform = receiver->transform();
    auto* script_transform_ref = transform.get_pointer<GC::Ref<RTCRtpScriptTransform>>();
    if (!script_transform_ref) {
        if (transform.has<Empty>())
            feed_decoded_audio(receiver_id, move(payload), rtp_timestamp);
        return;
    }
    auto& script_transform = **script_transform_ref;
    if (!script_transform.has_frame_written_callback()) {
        script_transform.set_on_frame_written([self = GC::Weak { *this }, receiver_id](ByteBuffer transformed_payload, u32, u8, u32 rtp_timestamp, u16) {
            if (self)
                self->feed_decoded_audio(receiver_id, move(transformed_payload), rtp_timestamp);
        });
    }

    script_transform.enqueue_encoded_audio_frame(move(payload), ssrc, payload_type, rtp_timestamp, sequence_number);
}

// Decode one decrypted opus packet and push PCM into the receiver's playback stream.
// FIXME: this is a shortcut around the spec — the right path is for MediaStreamTrack to
//        carry the audio data and HTMLMediaElement.srcObject playback to drain it.
void RTCPeerConnection::feed_decoded_audio(u64 receiver_id, ByteBuffer payload, u32 rtp_timestamp)
{
    if (m_is_closed)
        return;
    auto playback_iter = m_receiver_audio_playbacks.find(receiver_id);
    auto* playback = playback_iter == m_receiver_audio_playbacks.end() ? nullptr : playback_iter->value.ptr();
    if (!playback) {
        dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: setting up audio playback for receiver={}", receiver_id);
        // Opus is fundamentally 48 kHz; channel count comes out of the first decoded block.
        auto sample_spec = Audio::SampleSpecification(48000, Audio::ChannelMap::stereo());
        auto decoder_or_err = Media::FFmpeg::FFmpegAudioDecoder::try_create(Media::CodecID::Opus, sample_spec, {});
        if (decoder_or_err.is_error()) {
            dbgln("RTCPeerConnection: opus decoder init failed for receiver={}: {}", receiver_id, decoder_or_err.error().description());
            return;
        }
        auto fresh = make<ReceiverAudioPlayback>();
        fresh->decoder = decoder_or_err.release_value();
        auto* fresh_ptr = fresh.ptr();
        m_receiver_audio_playbacks.set(receiver_id, move(fresh));
        playback = fresh_ptr;

        auto data_callback = [buffer_state = playback->buffer](Span<float> buffer, MonotonicTime) -> ReadonlySpan<float> {
            MutexLocker locker(buffer_state->mutex);
            auto available = buffer_state->samples.size();
            auto take = AK::min(available, buffer.size());
            memcpy(buffer.data(), buffer_state->samples.data(), take * sizeof(float));
            buffer_state->samples.remove(0, take);
            // Pad the rest of the buffer with silence so the output keeps requesting
            // data instead of treating a short span as the end of the stream.
            if (take < buffer.size())
                memset(buffer.data() + take, 0, (buffer.size() - take) * sizeof(float));
            return buffer;
        };
        constexpr u32 target_latency_ms = 100;
        auto promise = Audio::PlaybackStream::create(Audio::OutputState::Suspended, target_latency_ms, move(data_callback));
        promise->when_resolved([self = GC::Weak { *this }, receiver_id](auto& stream) {
            if (!self || self->m_is_closed)
                return;
            auto entry = self->m_receiver_audio_playbacks.find(receiver_id);
            if (entry == self->m_receiver_audio_playbacks.end())
                return;
            auto* playback = entry->value.ptr();
            playback->playback_stream = stream;
            dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: playback stream resolved receiver={} spec={}", receiver_id, stream->sample_specification());
            auto converter_or_err = Media::FFmpeg::FFmpegAudioConverter::try_create();
            if (converter_or_err.is_error()) {
                dbgln("RTCPeerConnection: audio converter init failed for receiver={}: {}", receiver_id, converter_or_err.error());
            } else {
                auto converter = converter_or_err.release_value();
                if (auto rc = converter->set_output_sample_specification(stream->sample_specification()); rc.is_error())
                    dbgln("RTCPeerConnection: converter set_output failed for receiver={}: {}", receiver_id, rc.error());
                else
                    playback->converter = move(converter);
            }
            (void)stream->resume();
        });
        promise->when_rejected([receiver_id](auto& error) {
            dbgln("RTCPeerConnection: playback stream init failed for receiver={}: {}", receiver_id, error);
        });
    }

    if (!playback->decoder)
        return;

    auto timestamp = AK::Duration::from_milliseconds(static_cast<i64>(rtp_timestamp / 48));
    Media::CodedFrame coded_frame { Media::CodecID::Opus, timestamp, timestamp, {}, Media::FrameFlags::Keyframe, MUST(FixedArray<u8>::create(payload.bytes())) };
    if (auto result = playback->decoder->receive_coded_data(coded_frame); result.is_error()) {
        dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: opus receive_coded_data failed: {}", result.error().description());
        return;
    }
    Media::AudioBlock block;
    if (auto result = playback->decoder->write_next_block(block); result.is_error()) {
        dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: write_next_block failed: {}", result.error().description());
        return;
    }
    if (block.is_empty()) {
        dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: write_next_block produced empty block");
        return;
    }
    // Drop frames until the playback stream is up — we need its sample-spec to set up the
    // resampler, and feeding mismatched-rate samples in the meantime would just stretch.
    if (!playback->converter)
        return;
    if (auto rc = playback->converter->push_block(block); rc.is_error()) {
        dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: audio convert failed: {}", rc.error());
        return;
    }
    while (true) {
        Media::AudioBlock converted;
        if (auto rc = playback->converter->retrieve_block(converted); rc.is_error()) {
            break;
        }
        if (converted.is_empty())
            continue;
        MutexLocker locker(playback->buffer->mutex);
        playback->channel_count = converted.channel_count();
        auto max_samples = static_cast<size_t>(converted.sample_specification().sample_rate()) * converted.channel_count() / 5;
        if (playback->buffer->samples.size() > max_samples)
            playback->buffer->samples.remove(0, playback->buffer->samples.size() - max_samples);
        auto base = playback->buffer->samples.size();
        playback->buffer->samples.resize(base + converted.sample_count());
        converted.copy_to_interleaved(playback->buffer->samples.span().slice(base));
        dbgln_if(WEBRTC_DEBUG, "RTCPeerConnection: feed_decoded_audio receiver={} samples={} channels={} spec={} buffer_total={}",
            receiver_id, converted.sample_count(), converted.channel_count(), converted.sample_specification(), playback->buffer->samples.size());
    }
}

static Bindings::RTCSdpType parse_sdp_type(StringView s)
{
    if (s == "answer"sv)
        return Bindings::RTCSdpType::Answer;
    if (s == "pranswer"sv)
        return Bindings::RTCSdpType::Pranswer;
    if (s == "rollback"sv)
        return Bindings::RTCSdpType::Rollback;
    return Bindings::RTCSdpType::Offer;
}

void RTCPeerConnection::on_create_offer_result_received(u64 request_id, bool ok, String sdp_type, String sdp, String, String error_message)
{
    auto promise = m_pending_description_requests.take(request_id);
    if (!promise.has_value())
        return;
    auto& realm = relevant_realm();
    auto& promise_ref = **promise;
    HTML::TemporaryExecutionContext context(realm);
    if (!ok) {
        WebIDL::reject_promise(realm, promise_ref, WebIDL::OperationError::create(Utf16String::from_utf8(error_message)));
        return;
    }
    m_last_created_offer = Utf16String::from_utf8(sdp);
    auto init = JS::Object::create(realm, realm.intrinsics().object_prototype());
    MUST(init->create_data_property("type"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), idl_enum_to_string(parse_sdp_type(sdp_type)))));
    MUST(init->create_data_property("sdp"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(sdp))));
    WebIDL::resolve_promise(realm, promise_ref, init);
}

void RTCPeerConnection::on_create_answer_result_received(u64 request_id, bool ok, String sdp_type, String sdp, String, String error_message)
{
    auto promise = m_pending_description_requests.take(request_id);
    if (!promise.has_value())
        return;
    auto& realm = relevant_realm();
    auto& promise_ref = **promise;
    HTML::TemporaryExecutionContext context(realm);
    if (!ok) {
        WebIDL::reject_promise(realm, promise_ref, WebIDL::OperationError::create(Utf16String::from_utf8(error_message)));
        return;
    }
    m_last_created_answer = Utf16String::from_utf8(sdp);
    auto init = JS::Object::create(realm, realm.intrinsics().object_prototype());
    MUST(init->create_data_property("type"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), idl_enum_to_string(parse_sdp_type(sdp_type)))));
    MUST(init->create_data_property("sdp"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(sdp))));
    WebIDL::resolve_promise(realm, promise_ref, init);
}

bool RTCPeerConnection::is_setting_local_description() const
{
    return any_of(m_pending_description_payloads, [](auto const& entry) { return entry.value.is_local; });
}

void RTCPeerConnection::on_set_local_description_result_received(u64 request_id, bool ok, String, String error_message)
{
    settle_local_description(request_id, ok, error_message);

    // The page has seen the description by now, along with whatever its promise's reactions did with it, so the
    // candidates gathered for it can follow.
    if (!is_setting_local_description()) {
        auto held_events = move(m_held_ice_events);
        for (auto& event : held_events)
            event();
    }
}

void RTCPeerConnection::settle_local_description(u64 request_id, bool ok, String const& error_message)
{
    auto promise = m_pending_void_requests.take(request_id);
    auto payload = m_pending_description_payloads.take(request_id);
    if (!promise.has_value())
        return;
    auto& realm = relevant_realm();
    auto& promise_ref = **promise;
    HTML::TemporaryExecutionContext context(realm);
    if (!ok) {
        WebIDL::reject_promise(realm, promise_ref, WebIDL::OperationError::create(Utf16String::from_utf8(error_message)));
        return;
    }
    if (payload.has_value() && payload->is_local) {
        auto description = RTCSessionDescription::create(RTCSessionDescriptionInit { .sdp = payload->sdp, .type = payload->type });
        switch (payload->type) {
        case Bindings::RTCSdpType::Offer:
        case Bindings::RTCSdpType::Pranswer:
            m_pending_local_description = description;
            break;
        case Bindings::RTCSdpType::Answer:
            m_current_local_description = description;
            m_current_remote_description = m_pending_remote_description;
            m_pending_local_description = nullptr;
            m_pending_remote_description = nullptr;
            m_restart_ice = false;
            break;
        case Bindings::RTCSdpType::Rollback:
            m_pending_local_description = nullptr;
            m_pending_remote_description = nullptr;
            break;
        }
    }
    WebIDL::resolve_promise(realm, promise_ref, JS::js_undefined());
}

void RTCPeerConnection::on_set_remote_description_result_received(u64 request_id, bool ok, String, String error_message)
{
    auto promise = m_pending_void_requests.take(request_id);
    auto payload = m_pending_description_payloads.take(request_id);
    if (!promise.has_value())
        return;
    auto& realm = relevant_realm();
    auto& promise_ref = **promise;
    HTML::TemporaryExecutionContext context(realm);
    if (!ok) {
        WebIDL::reject_promise(realm, promise_ref, WebIDL::OperationError::create(Utf16String::from_utf8(error_message)));
        return;
    }
    if (payload.has_value() && !payload->is_local) {
        auto description = RTCSessionDescription::create(RTCSessionDescriptionInit { .sdp = payload->sdp, .type = payload->type });
        switch (payload->type) {
        case Bindings::RTCSdpType::Offer:
        case Bindings::RTCSdpType::Pranswer:
            m_pending_remote_description = description;
            break;
        case Bindings::RTCSdpType::Answer:
            m_current_remote_description = description;
            m_current_local_description = m_pending_local_description;
            m_pending_local_description = nullptr;
            m_pending_remote_description = nullptr;
            m_restart_ice = false;
            break;
        case Bindings::RTCSdpType::Rollback:
            m_pending_local_description = nullptr;
            m_pending_remote_description = nullptr;
            break;
        }
    }
    WebIDL::resolve_promise(realm, promise_ref, JS::js_undefined());
}

void RTCPeerConnection::on_add_ice_candidate_result_received(u64 request_id, bool ok, String, String error_message)
{
    auto promise = m_pending_void_requests.take(request_id);
    if (!promise.has_value())
        return;
    auto& realm = relevant_realm();
    auto& promise_ref = **promise;
    HTML::TemporaryExecutionContext context(realm);
    if (!ok)
        WebIDL::reject_promise(realm, promise_ref, WebIDL::OperationError::create(Utf16String::from_utf8(error_message)));
    else
        WebIDL::resolve_promise(realm, promise_ref, JS::js_undefined());
}

void RTCPeerConnection::on_remote_data_channel_received(u64 channel_id, String label, bool ordered, Optional<u16> max_packet_life_time, Optional<u16> max_retransmits, String protocol, bool negotiated, Optional<u16> id)
{
    if (m_is_closed)
        return;
    HTML::TemporaryExecutionContext context(relevant_realm());
    auto channel = RTCDataChannel::create(m_global_object);
    channel->set_channel_id(channel_id);
    channel->set_label(Utf16String::from_utf8(label));
    channel->set_ordered(ordered);
    channel->set_max_packet_life_time(max_packet_life_time);
    channel->set_max_retransmits(max_retransmits);
    channel->set_protocol(Utf16String::from_utf8(protocol));
    channel->set_negotiated(negotiated);
    channel->set_id(id);
    channel->set_ready_state(Bindings::RTCDataChannelState::Open);
    m_data_channels.append(channel);
    m_data_channels_by_id.set(channel_id, channel);
    WebRTCAgent::the().register_data_channel(channel_id, channel);
    RTCDataChannelEventInit init { {}, channel };
    dispatch_event(RTCDataChannelEvent::create(HTML::EventNames::datachannel, init, HighResolutionTime::current_high_resolution_time(relevant_global_object())));
}

#define EVENT_HANDLER(name, event_name)                                                                                             \
    void RTCPeerConnection::set_##name(WebIDL::CallbackType* cb) { set_event_handler_attribute(HTML::EventNames::event_name, cb); } \
    WebIDL::CallbackType* RTCPeerConnection::name() { return event_handler_attribute(HTML::EventNames::event_name); }

EVENT_HANDLER(onnegotiationneeded, negotiationneeded)
EVENT_HANDLER(onicecandidate, icecandidate)
EVENT_HANDLER(onicecandidateerror, icecandidateerror)
EVENT_HANDLER(onsignalingstatechange, signalingstatechange)
EVENT_HANDLER(oniceconnectionstatechange, iceconnectionstatechange)
EVENT_HANDLER(onicegatheringstatechange, icegatheringstatechange)
EVENT_HANDLER(onconnectionstatechange, connectionstatechange)
EVENT_HANDLER(ontrack, track)
EVENT_HANDLER(ondatachannel, datachannel)

#undef EVENT_HANDLER

}
