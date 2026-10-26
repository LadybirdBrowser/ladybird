/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Mutex.h>
#include <AK/Utf16String.h>
#include <LibAudio/Forward.h>
#include <LibMedia/Forward.h>
#include <LibWeb/Bindings/Forward.h>
#include <LibWeb/Bindings/MediaStreamTrack.h>
#include <LibWeb/Bindings/RTCPeerConnection.h>
#include <LibWeb/Bindings/RTCRtpTransceiver.h>
#include <LibWeb/Bindings/RTCSessionDescription.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebRTC {

class RTCDataChannel;
class RTCRtpReceiver;
class RTCRtpSender;
class RTCRtpTransceiver;
class RTCSctpTransport;
class RTCSessionDescription;

using Bindings::RTCAnswerOptions;
using Bindings::RTCConfiguration;
using Bindings::RTCDataChannelInit;
using Bindings::RTCIceCandidateInit;
using Bindings::RTCIceServer;
using Bindings::RTCLocalSessionDescriptionInit;
using Bindings::RTCOfferAnswerOptions;
using Bindings::RTCOfferOptions;
using Bindings::RTCRtpTransceiverInit;
using Bindings::RTCSessionDescriptionInit;

class RTCPeerConnection final : public DOM::EventTarget {
    WEB_WRAPPABLE(RTCPeerConnection, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(RTCPeerConnection);

public:
    static WebIDL::ExceptionOr<GC::Ref<RTCPeerConnection>> create_for_constructor(JS::Object& relevant_global_object, RTCConfiguration const& = {});

    virtual void finalize() override;

    JS::Realm& relevant_realm() const;
    JS::Object& relevant_global_object() const;
    virtual GC::Ptr<Bindings::Wrappable> relevant_global_impl() const override { return m_global_object; }

    Bindings::RTCSignalingState signaling_state() const { return m_signaling_state; }
    Bindings::RTCIceGatheringState ice_gathering_state() const { return m_ice_gathering_state; }
    Bindings::RTCIceConnectionState ice_connection_state() const { return m_ice_connection_state; }
    Bindings::RTCPeerConnectionState connection_state() const { return m_connection_state; }
    bool is_closed() const { return m_is_closed; }

    GC::RootVector<GC::Ref<RTCRtpTransceiver>> collect_transceivers() const;
    void update_negotiation_needed_flag();

    GC::Ref<WebIDL::Promise> create_offer(RTCOfferOptions const&);
    GC::Ref<WebIDL::Promise> create_an_offer();
    RTCConfiguration const& get_configuration() const { return m_configuration; }
    void restart_ice();
    bool m_restart_ice { false };
    GC::Ref<WebIDL::Promise> create_answer(RTCAnswerOptions const&);
    GC::Ref<WebIDL::Promise> create_an_answer();
    GC::Ref<WebIDL::Promise> set_local_description(RTCLocalSessionDescriptionInit const&);
    GC::Ref<WebIDL::Promise> set_remote_description(RTCSessionDescriptionInit const&);
    GC::Ref<WebIDL::Promise> add_ice_candidate(RTCIceCandidateInit const&);

    GC::Ref<WebIDL::Promise> set_a_local_description(Bindings::RTCSdpType, Utf16String const& sdp);
    GC::Ref<WebIDL::Promise> set_a_remote_description(RTCSessionDescriptionInit const&);

    GC::Ptr<RTCSessionDescription> local_description() const;
    GC::Ptr<RTCSessionDescription> current_local_description() const { return m_current_local_description; }
    GC::Ptr<RTCSessionDescription> pending_local_description() const { return m_pending_local_description; }
    GC::Ptr<RTCSessionDescription> remote_description() const;
    GC::Ptr<RTCSessionDescription> current_remote_description() const { return m_current_remote_description; }
    GC::Ptr<RTCSessionDescription> pending_remote_description() const { return m_pending_remote_description; }

    GC::RootVector<GC::Ref<RTCRtpSender>> get_senders() const { return collect_senders(); }
    GC::RootVector<GC::Ref<RTCRtpReceiver>> get_receivers() const;
    GC::RootVector<GC::Ref<RTCRtpTransceiver>> get_transceivers() const { return collect_transceivers(); }

    WebIDL::ExceptionOr<GC::Ref<RTCRtpSender>> add_track(GC::Ref<MediaCapture::MediaStreamTrack>, GC::Ref<MediaCapture::MediaStream>);
    WebIDL::ExceptionOr<GC::Ref<RTCRtpSender>> add_track(GC::Ref<MediaCapture::MediaStreamTrack>, GC::RootVector<GC::Ref<MediaCapture::MediaStream>> const&);
    WebIDL::ExceptionOr<GC::Ref<RTCRtpTransceiver>> add_transceiver(Variant<GC::Ref<MediaCapture::MediaStreamTrack>, Utf16String> const& track_or_kind, RTCRtpTransceiverInit const& = {});
    GC::Ptr<RTCSctpTransport> sctp() const { return m_sctp; }
    WebIDL::ExceptionOr<GC::Ref<RTCDataChannel>> create_data_channel(Utf16String const& label, RTCDataChannelInit const&);

    GC::RootVector<GC::Ref<RTCRtpSender>> collect_senders() const;
    WebIDL::ExceptionOr<void> remove_track(GC::Ref<RTCRtpSender>);
    void stop_sender(RTCRtpSender&);

    GC::Ref<WebIDL::Promise> get_stats(GC::Ptr<MediaCapture::MediaStreamTrack>);

    void close();
    void close_for_document_destruction();
    void on_helper_died();

    u64 pc_id() const { return m_pc_id; }

    // Inbound IPC event hooks (called by WebRTCAgent's dispatch).
    void on_stats_received(u64 request_id, String reports, String error);
    void on_signaling_state_event(String state);
    void on_connection_state_event(String state);
    void on_ice_gathering_state_event(String state);
    void on_ice_connection_state_event(String state);
    void on_ice_candidate_received(Optional<String> candidate, Optional<String> sdp_mid, Optional<u32> sdp_mline_index);
    void on_ice_candidate_error_received(Optional<String> address, Optional<u16> port, String url, u16 error_code, String error_text);
    void on_negotiation_needed_received();
    void on_remote_track_added(u64 receiver_id, u64 transceiver_id, String kind, Vector<String> stream_ids);
    void on_remote_track_ended(u64 receiver_id);
    void on_encoded_audio_frame_received(u64 receiver_id, u32 ssrc, u32 rtp_timestamp, u16 sequence_number, u8 payload_type, ByteBuffer payload);
    void on_audio_track_ssrc_assigned(u64 sender_id, u32 ssrc);
    void on_create_offer_result_received(u64 request_id, bool ok, String sdp_type, String sdp, String error_kind, String error_message);
    void on_create_answer_result_received(u64 request_id, bool ok, String sdp_type, String sdp, String error_kind, String error_message);
    void on_set_local_description_result_received(u64 request_id, bool ok, String error_kind, String error_message);
    void on_set_remote_description_result_received(u64 request_id, bool ok, String error_kind, String error_message);
    void on_add_ice_candidate_result_received(u64 request_id, bool ok, String error_kind, String error_message);
    void on_remote_data_channel_received(u64 channel_id, String label, bool ordered, Optional<u16> max_packet_life_time, Optional<u16> max_retransmits, String protocol, bool negotiated, Optional<u16> id);

#define EVENT_HANDLER(name)                 \
    void set_##name(WebIDL::CallbackType*); \
    WebIDL::CallbackType* name();
    EVENT_HANDLER(onnegotiationneeded)
    EVENT_HANDLER(onicecandidate)
    EVENT_HANDLER(onicecandidateerror)
    EVENT_HANDLER(onsignalingstatechange)
    EVENT_HANDLER(oniceconnectionstatechange)
    EVENT_HANDLER(onicegatheringstatechange)
    EVENT_HANDLER(onconnectionstatechange)
    EVENT_HANDLER(ontrack)
    EVENT_HANDLER(ondatachannel)
#undef EVENT_HANDLER

private:
    explicit RTCPeerConnection(GC::Ref<DOM::EventTarget> relevant_global_object, RTCConfiguration);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    void close_the_connection_algorithm(bool disappear);
    GC::Ref<WebIDL::Promise> chain_operation(GC::Ref<WebIDL::ReactionSteps>);
    GC::Ptr<WebIDL::Promise> m_last_operation;
    GC::Ref<WebIDL::Promise> create_offer_impl(RTCOfferOptions const&);
    GC::Ref<WebIDL::Promise> create_answer_impl(RTCAnswerOptions const&);
    GC::Ref<WebIDL::Promise> set_local_description_impl(RTCLocalSessionDescriptionInit const&);
    GC::Ref<WebIDL::Promise> set_remote_description_impl(RTCSessionDescriptionInit const&);
    GC::Ref<WebIDL::Promise> add_ice_candidate_impl(RTCIceCandidateInit const&);

    // The global this connection was created for; used to recover the realm for
    // promise/event work triggered by IPC.
    GC::Ref<DOM::EventTarget> m_global_object;

    RTCConfiguration m_configuration;

    // [[SignalingState]]
    Bindings::RTCSignalingState m_signaling_state { Bindings::RTCSignalingState::Stable };
    // [[IceGatheringState]]
    Bindings::RTCIceGatheringState m_ice_gathering_state { Bindings::RTCIceGatheringState::New };
    // [[IceConnctionState]]
    Bindings::RTCIceConnectionState m_ice_connection_state { Bindings::RTCIceConnectionState::New };
    // [[ConnctionState]]
    Bindings::RTCPeerConnectionState m_connection_state { Bindings::RTCPeerConnectionState::New };
    // [[IsClosed]]
    bool m_is_closed { false };
    // [[SctpTransport]]
    GC::Ptr<RTCSctpTransport> m_sctp;
    // [[Transceivers]]
    Vector<GC::Ref<RTCRtpTransceiver>> m_transceivers;
    // [[DataChannels]]
    Vector<GC::Ref<RTCDataChannel>> m_data_channels;
    HashMap<u64, GC::Ref<RTCDataChannel>> m_data_channels_by_id;
    // [[LastCreatedOffer]]
    Utf16String m_last_created_offer;
    u64 m_pc_id { 0 };
    HashMap<u64, GC::Root<WebIDL::Promise>> m_pending_void_requests;
    HashMap<u64, GC::Root<WebIDL::Promise>> m_pending_description_requests;

    HashMap<String, GC::Ref<MediaCapture::MediaStream>> m_remote_streams;
    HashMap<u64, GC::Ref<RTCRtpReceiver>> m_remote_receivers_by_id;

public:
    GC::Ptr<MediaCapture::MediaStream> find_or_create_remote_stream_for_msid(String const&);

    // Start capturing audio only once there is a sender to feed.
    void on_sender_track_changed(RTCRtpSender&);
    void on_sender_transform_changed(RTCRtpSender&);

private:
    struct PendingDescription {
        Bindings::RTCSdpType type;
        Utf16String sdp;
        bool is_local;
    };
    HashMap<u64, PendingDescription> m_pending_description_payloads;
    bool is_setting_local_description() const;
    void settle_local_description(u64 request_id, bool ok, String const& error_message);
    // ICE events the helper sent before the local description they follow was set; see on_ice_candidate_received().
    Vector<Function<void()>> m_held_ice_events;
    // [[LastCreatedAnswer]]
    Utf16String m_last_created_answer;
    // [[CurrentLocalDescription]]
    GC::Ptr<RTCSessionDescription> m_current_local_description;
    // [[PendingLocalDescription]]
    GC::Ptr<RTCSessionDescription> m_pending_local_description;
    // [[CurrentRemoteDescription]]
    GC::Ptr<RTCSessionDescription> m_current_remote_description;
    // [[PendingRemoteDescription]]
    GC::Ptr<RTCSessionDescription> m_pending_remote_description;

    // Per-remote-receiver playback stack. We bypass the spec's MediaStream → audio-element
    // path entirely and just opus-decode + push PCM straight to libmedia's audio output.
    // Once we wire MediaStreamTrack as a real audio source, this should go away.
    struct PlaybackBuffer : public AtomicRefCounted<PlaybackBuffer> {
        Mutex mutex;
        Vector<float> samples;
    };
    struct ReceiverAudioPlayback {
        AK_ALLOC_WITH_KMALLOC;

        OwnPtr<Media::AudioDecoder> decoder;
        OwnPtr<Media::FFmpeg::FFmpegAudioConverter> converter;
        RefPtr<Audio::PlaybackStream> playback_stream;
        NonnullRefPtr<PlaybackBuffer> buffer = adopt_ref(*new PlaybackBuffer);
        u8 channel_count { 0 };
        bool started { false };
    };
    HashMap<u64, NonnullOwnPtr<ReceiverAudioPlayback>> m_receiver_audio_playbacks;
    void feed_decoded_audio(u64 receiver_id, ByteBuffer payload, u32 rtp_timestamp);

    // The track may outlive this connection, so its producer callback owns this state independently.
    struct OutgoingAudioProducerState : public AtomicRefCounted<OutgoingAudioProducerState> {
        OwnPtr<Media::FFmpeg::FFmpegAudioConverter> converter;
        u64 source_frames { 0 };
        Vector<float> float_accumulator;
        Atomic<size_t> pending_frames { 0 };
    };
    // Main-thread state for encoding and routing the producer's 20 ms PCM frames.
    struct OutgoingAudioPipeline {
        AK_ALLOC_WITH_KMALLOC;

        RefPtr<MediaCapture::AudioFrameSink> sink;
        GC::Ptr<MediaCapture::MediaStreamTrack> track;
        OwnPtr<Media::FFmpeg::FFmpegAudioEncoder> encoder;
        u64 generation { 0 };
        u32 next_rtp_timestamp { 0 };
        u16 next_sequence_number { 0 };
    };
    HashMap<u64, NonnullOwnPtr<OutgoingAudioPipeline>> m_outgoing_audio_pipelines;
    HashTable<u64> m_requested_audio_senders;
    HashTable<u64> m_ready_audio_senders;
    void start_outgoing_audio_for_sender(GC::Ref<RTCRtpSender>);
    void encode_and_route_outgoing_pcm(u64 sender_id, ByteBuffer pcm_s16le);
    void on_outgoing_encrypted_frame(u64 sender_id, ByteBuffer payload, u32 rtp_timestamp, u16 sequence_number, u8 payload_type);
};

}
