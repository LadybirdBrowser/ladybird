/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibIPC/ConnectionToServer.h>
#include <WebRTCClient/WebRTCClientClientEndpoint.h>
#include <WebRTCClient/WebRTCClientServerEndpoint.h>

namespace WebRTCClient {

class Client final
    : public IPC::ConnectionToServer<WebRTCClientClientEndpoint, WebRTCClientServerEndpoint>
    , public WebRTCClientClientEndpoint {
    C_OBJECT_ABSTRACT(Client);

public:
    using InitTransport = Messages::WebRTCClientServer::InitTransport;

    Client(NonnullOwnPtr<IPC::Transport>);

    Function<void()> on_death;
    Function<void(u64, u64, String, String)> on_stats_result;

    Function<void(u64 pc_id, String state)> on_signaling_state_change;
    Function<void(u64 pc_id, String state)> on_connection_state_change;
    Function<void(u64 pc_id, String state)> on_ice_gathering_state_change;
    Function<void(u64 pc_id, String state)> on_ice_connection_state_change;
    Function<void(u64 pc_id, Optional<String> candidate, Optional<String> sdp_mid, Optional<u32> sdp_mline_index)> on_ice_candidate_event;
    Function<void(u64 pc_id, Optional<String> address, Optional<u16> port, String url, u16 error_code, String error_text)> on_ice_candidate_error_event;
    Function<void(u64 pc_id)> on_negotiation_needed_event;
    Function<void(u64 pc_id, u64 receiver_id, u64 transceiver_id, String kind, Vector<String> stream_ids)> on_track_event;
    Function<void(u64 pc_id, u64 receiver_id)> on_track_ended_event;
    Function<void(u64 pc_id, u64 receiver_id, u32 ssrc, u32 rtp_timestamp, u16 sequence_number, u8 payload_type, ByteBuffer payload)> on_encoded_audio_frame_event;
    Function<void(u64 pc_id, u64 sender_id, u32 ssrc)> on_audio_track_added_event;

    Function<void(u64 pc_id, u64 request_id, bool ok, String sdp_type, String sdp, String error_kind, String error_message)> on_create_offer_completion;
    Function<void(u64 pc_id, u64 request_id, bool ok, String sdp_type, String sdp, String error_kind, String error_message)> on_create_answer_completion;
    Function<void(u64 pc_id, u64 request_id, bool ok, String error_kind, String error_message)> on_set_local_description_completion;
    Function<void(u64 pc_id, u64 request_id, bool ok, String error_kind, String error_message)> on_set_remote_description_completion;
    Function<void(u64 pc_id, u64 request_id, bool ok, String error_kind, String error_message)> on_add_ice_candidate_completion;

    Function<void(u64 pc_id, u64 channel_id, String label, bool ordered, Optional<u16> max_packet_life_time, Optional<u16> max_retransmits, String protocol, bool negotiated, Optional<u16> id)> on_data_channel_event;
    Function<void(u64 channel_id)> on_data_channel_open_event;
    Function<void(u64 channel_id, String data)> on_data_channel_message_text_event;
    Function<void(u64 channel_id, ByteBuffer data)> on_data_channel_message_binary_event;
    Function<void(u64 channel_id)> on_data_channel_closing_event;
    Function<void(u64 channel_id)> on_data_channel_closed_event;
    Function<void(u64 channel_id, String error)> on_data_channel_error_event;
    Function<void(u64 channel_id)> on_data_channel_buffered_amount_low_event;
    Function<void(u64 request_id, String host)> on_resolve_host;

private:
    virtual void die() override;
    virtual void on_stats(u64, u64, String, String) override;

    virtual void on_signaling_state(u64 pc_id, String state) override;
    virtual void on_connection_state(u64 pc_id, String state) override;
    virtual void on_ice_gathering_state(u64 pc_id, String state) override;
    virtual void on_ice_connection_state(u64 pc_id, String state) override;
    virtual void on_ice_candidate(u64 pc_id, Optional<String> candidate, Optional<String> sdp_mid, Optional<u32> sdp_mline_index) override;
    virtual void on_ice_candidate_error(u64 pc_id, Optional<String> address, Optional<u16> port, String url, u16 error_code, String error_text) override;
    virtual void on_negotiation_needed(u64 pc_id) override;
    virtual void on_track(u64 pc_id, u64 receiver_id, u64 transceiver_id, String kind, Vector<String> stream_ids) override;
    virtual void on_track_ended(u64 pc_id, u64 receiver_id) override;
    virtual void on_encoded_audio_frame(u64 pc_id, u64 receiver_id, u32 ssrc, u32 rtp_timestamp, u16 sequence_number, u8 payload_type, ByteBuffer payload) override;
    virtual void on_audio_track_added(u64 pc_id, u64 sender_id, u32 ssrc) override;

    virtual void on_create_offer_result(u64 pc_id, u64 request_id, bool ok, String sdp_type, String sdp, String error_kind, String error_message) override;
    virtual void on_create_answer_result(u64 pc_id, u64 request_id, bool ok, String sdp_type, String sdp, String error_kind, String error_message) override;
    virtual void on_set_local_description_result(u64 pc_id, u64 request_id, bool ok, String error_kind, String error_message) override;
    virtual void on_set_remote_description_result(u64 pc_id, u64 request_id, bool ok, String error_kind, String error_message) override;
    virtual void on_add_ice_candidate_result(u64 pc_id, u64 request_id, bool ok, String error_kind, String error_message) override;

    virtual void on_data_channel(u64 pc_id, u64 channel_id, String label, bool ordered, Optional<u16> max_packet_life_time, Optional<u16> max_retransmits, String protocol, bool negotiated, Optional<u16> id) override;
    virtual void on_data_channel_open(u64 channel_id) override;
    virtual void on_data_channel_message_text(u64 channel_id, String data) override;
    virtual void on_data_channel_message_binary(u64 channel_id, ByteBuffer data) override;
    virtual void on_data_channel_closing(u64 channel_id) override;
    virtual void on_data_channel_closed(u64 channel_id) override;
    virtual void on_data_channel_error(u64 channel_id, String error) override;
    virtual void on_data_channel_buffered_amount_low(u64 channel_id) override;
    virtual void resolve_host(u64 request_id, String host) override;
};

}
