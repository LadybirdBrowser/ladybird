/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/MediaStreamTrack.h>
#include <LibWeb/Bindings/RTCRtpTransceiver.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebRTC {

class RTCPeerConnection;
class RTCRtpReceiver;
class RTCRtpSender;

class RTCRtpTransceiver final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(RTCRtpTransceiver, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(RTCRtpTransceiver);

public:
    static GC::Ref<RTCRtpTransceiver> create(RTCPeerConnection&, GC::Ref<RTCRtpSender>, GC::Ref<RTCRtpReceiver>, Bindings::RTCRtpTransceiverDirection, Bindings::MediaStreamTrackKind);
    virtual ~RTCRtpTransceiver() override;

    GC::Ref<RTCRtpSender> sender() const { return m_sender; }
    GC::Ref<RTCRtpReceiver> receiver() const { return m_receiver; }
    Bindings::RTCRtpTransceiverDirection direction() const { return m_direction; }
    void set_direction(Bindings::RTCRtpTransceiverDirection);
    Bindings::MediaStreamTrackKind kind() const { return m_kind; }
    Optional<Bindings::RTCRtpTransceiverDirection> current_direction() const { return m_current_direction; }
    bool is_stopped() const { return m_stopped; }
    bool is_stopping() const { return m_stopping; }

    u64 transceiver_id() const { return m_transceiver_id; }
    void set_transceiver_id(u64 id) { m_transceiver_id = id; }

    Optional<Bindings::RTCRtpTransceiverDirection> fired_direction() const { return m_fired_direction; }
    void set_fired_direction(Optional<Bindings::RTCRtpTransceiverDirection> d) { m_fired_direction = d; }

    bool is_receptive() const { return m_receptive; }
    void set_receptive(bool v) { m_receptive = v; }

    WebIDL::ExceptionOr<void> stop_method();

    void stop_sending_and_receiving(bool disappear = false);
    void stop(bool disappear = false);

private:
    explicit RTCRtpTransceiver(RTCPeerConnection&, GC::Ref<RTCRtpSender>, GC::Ref<RTCRtpReceiver>, Bindings::RTCRtpTransceiverDirection, Bindings::MediaStreamTrackKind);
    virtual void visit_edges(JS::Cell::Visitor&) override;

    // [[Connection]]
    GC::Ref<RTCPeerConnection> m_connection;
    // [[Sender]]
    GC::Ref<RTCRtpSender> m_sender;
    // [[Receiver]]
    GC::Ref<RTCRtpReceiver> m_receiver;
    // [[Direction]]
    Bindings::RTCRtpTransceiverDirection m_direction;
    // [[CurrentDirection]]
    Optional<Bindings::RTCRtpTransceiverDirection> m_current_direction;
    // [[Stopping]]
    bool m_stopping { false };
    // [[Stopped]]
    bool m_stopped { false };
    // [[Receptive]]
    bool m_receptive { false };

    Bindings::MediaStreamTrackKind m_kind;
    u64 m_transceiver_id { 0 };
    // [[FiredDirection]]
    Optional<Bindings::RTCRtpTransceiverDirection> m_fired_direction;
};

}
