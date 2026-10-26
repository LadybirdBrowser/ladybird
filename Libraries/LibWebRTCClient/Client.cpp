/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibWebRTCClient/Client.h>

namespace WebRTCClient {

Client::Client(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<WebRTCClientClientEndpoint, WebRTCClientServerEndpoint>(*this, move(transport))
{
}

void Client::on_stats(u64 pc_id, u64 request_id, String reports, String error)
{
    if (on_stats_result)
        on_stats_result(pc_id, request_id, move(reports), move(error));
}

void Client::die()
{
    if (auto callback = move(on_death))
        callback();
}

void Client::on_signaling_state(u64 pc_id, String state)
{
    if (on_signaling_state_change)
        on_signaling_state_change(pc_id, move(state));
}

void Client::on_connection_state(u64 pc_id, String state)
{
    if (on_connection_state_change)
        on_connection_state_change(pc_id, move(state));
}

void Client::on_ice_gathering_state(u64 pc_id, String state)
{
    if (on_ice_gathering_state_change)
        on_ice_gathering_state_change(pc_id, move(state));
}

void Client::on_ice_connection_state(u64 pc_id, String state)
{
    if (on_ice_connection_state_change)
        on_ice_connection_state_change(pc_id, move(state));
}

void Client::on_ice_candidate(u64 pc_id, Optional<String> candidate, Optional<String> sdp_mid, Optional<u32> sdp_mline_index)
{
    if (on_ice_candidate_event)
        on_ice_candidate_event(pc_id, move(candidate), move(sdp_mid), sdp_mline_index);
}

void Client::on_ice_candidate_error(u64 pc_id, Optional<String> address, Optional<u16> port, String url, u16 error_code, String error_text)
{
    if (on_ice_candidate_error_event)
        on_ice_candidate_error_event(pc_id, move(address), port, move(url), error_code, move(error_text));
}

void Client::on_negotiation_needed(u64 pc_id)
{
    if (on_negotiation_needed_event)
        on_negotiation_needed_event(pc_id);
}

void Client::on_track(u64 pc_id, u64 receiver_id, u64 transceiver_id, String kind, Vector<String> stream_ids)
{
    if (on_track_event)
        on_track_event(pc_id, receiver_id, transceiver_id, move(kind), move(stream_ids));
}

void Client::on_track_ended(u64 pc_id, u64 receiver_id)
{
    if (on_track_ended_event)
        on_track_ended_event(pc_id, receiver_id);
}

void Client::on_encoded_audio_frame(u64 pc_id, u64 receiver_id, u32 ssrc, u32 rtp_timestamp, u16 sequence_number, u8 payload_type, ByteBuffer payload)
{
    if (on_encoded_audio_frame_event)
        on_encoded_audio_frame_event(pc_id, receiver_id, ssrc, rtp_timestamp, sequence_number, payload_type, move(payload));
}

void Client::on_audio_track_added(u64 pc_id, u64 sender_id, u32 ssrc)
{
    if (on_audio_track_added_event)
        on_audio_track_added_event(pc_id, sender_id, ssrc);
}

void Client::on_create_offer_result(u64 pc_id, u64 request_id, bool ok, String sdp_type, String sdp, String error_kind, String error_message)
{
    if (on_create_offer_completion)
        on_create_offer_completion(pc_id, request_id, ok, move(sdp_type), move(sdp), move(error_kind), move(error_message));
}

void Client::on_create_answer_result(u64 pc_id, u64 request_id, bool ok, String sdp_type, String sdp, String error_kind, String error_message)
{
    if (on_create_answer_completion)
        on_create_answer_completion(pc_id, request_id, ok, move(sdp_type), move(sdp), move(error_kind), move(error_message));
}

void Client::on_set_local_description_result(u64 pc_id, u64 request_id, bool ok, String error_kind, String error_message)
{
    if (on_set_local_description_completion)
        on_set_local_description_completion(pc_id, request_id, ok, move(error_kind), move(error_message));
}

void Client::on_set_remote_description_result(u64 pc_id, u64 request_id, bool ok, String error_kind, String error_message)
{
    if (on_set_remote_description_completion)
        on_set_remote_description_completion(pc_id, request_id, ok, move(error_kind), move(error_message));
}

void Client::on_add_ice_candidate_result(u64 pc_id, u64 request_id, bool ok, String error_kind, String error_message)
{
    if (on_add_ice_candidate_completion)
        on_add_ice_candidate_completion(pc_id, request_id, ok, move(error_kind), move(error_message));
}

void Client::on_data_channel(u64 pc_id, u64 channel_id, String label, bool ordered, Optional<u16> max_packet_life_time, Optional<u16> max_retransmits, String protocol, bool negotiated, Optional<u16> id)
{
    if (on_data_channel_event)
        on_data_channel_event(pc_id, channel_id, move(label), ordered, max_packet_life_time, max_retransmits, move(protocol), negotiated, id);
}

void Client::on_data_channel_open(u64 channel_id)
{
    if (on_data_channel_open_event)
        on_data_channel_open_event(channel_id);
}

void Client::on_data_channel_message_text(u64 channel_id, String data)
{
    if (on_data_channel_message_text_event)
        on_data_channel_message_text_event(channel_id, move(data));
}

void Client::on_data_channel_message_binary(u64 channel_id, ByteBuffer data)
{
    if (on_data_channel_message_binary_event)
        on_data_channel_message_binary_event(channel_id, move(data));
}

void Client::on_data_channel_closing(u64 channel_id)
{
    if (on_data_channel_closing_event)
        on_data_channel_closing_event(channel_id);
}

void Client::on_data_channel_closed(u64 channel_id)
{
    if (on_data_channel_closed_event)
        on_data_channel_closed_event(channel_id);
}

void Client::on_data_channel_error(u64 channel_id, String error)
{
    if (on_data_channel_error_event)
        on_data_channel_error_event(channel_id, move(error));
}

void Client::on_data_channel_buffered_amount_low(u64 channel_id)
{
    if (on_data_channel_buffered_amount_low_event)
        on_data_channel_buffered_amount_low_event(channel_id);
}

void Client::resolve_host(u64 request_id, String host)
{
    if (on_resolve_host)
        on_resolve_host(request_id, move(host));
    else
        async_resolved_host(request_id, {});
}

}
