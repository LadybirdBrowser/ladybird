/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibMediaCodecs/CodecID.h>

namespace Web::WebRTC {

// An encoded frame on its way through a script transform: to the worker that transforms it, and back again.
struct EncodedFrame {
    ByteBuffer payload;
    u32 ssrc { 0 };
    u8 payload_type { 0 };
    u32 rtp_timestamp { 0 };
    // An audio frame is the payload of one RTP packet.
    u16 sequence_number { 0 };

    struct Video {
        Media::CodecID codec { Media::CodecID::Unknown };
        bool is_key_frame { false };
        // Counts the frames given to the transform, so that one it doesn't hand back is missed.
        u64 frame_id { 0 };
        // Which of the sender's tracks a frame on its way out was encoded from, so that a frame the transform hands
        // back after the track was replaced is not sent as the new track's. Zero for a frame that was received.
        u64 sender_pipeline { 0 };
        bool is_outgoing() const { return sender_pipeline != 0; }
        // The source a received frame came from, so that one the transform hands back after the receiver was given
        // another source is not taken for the new source's, whose frames are counted from their own start.
        u64 receiver_id { 0 };
    };
    Optional<Video> video;
};

}
