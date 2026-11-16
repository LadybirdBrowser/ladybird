/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/MediaCapture/VideoTrackEncoder.h>
#include <LibWeb/WebRTC/RTCRtpCapabilities.h>

namespace Web::WebRTC {

// NB: What is listed here has to agree with what the backend offers, which is register_codecs() and build_api() in
//     LibWebRTC's peer.rs.

static RTCRtpCodec codec(Utf16String mime_type, u32 clock_rate, Optional<u16> channels = {}, Optional<Utf16String> sdp_fmtp_line = {})
{
    return { .channels = channels, .clock_rate = clock_rate, .mime_type = move(mime_type), .sdp_fmtp_line = move(sdp_fmtp_line) };
}

static RTCRtpCodec opus()
{
    return codec("audio/opus"_utf16, 48000, 2, "minptime=10;useinbandfec=1"_utf16);
}

static Vector<RTCRtpCodec> video_codecs(Media::CodecID codec_id)
{
    static constexpr u32 VIDEO_CLOCK_RATE = 90'000;
    Vector<RTCRtpCodec> codecs;
    switch (codec_id) {
    case Media::CodecID::VP8:
        codecs.append(codec("video/VP8"_utf16, VIDEO_CLOCK_RATE));
        break;
    case Media::CodecID::VP9:
        codecs.append(codec("video/VP9"_utf16, VIDEO_CLOCK_RATE, {}, "profile-id=0"_utf16));
        break;
    case Media::CodecID::H264:
        for (auto fmtp_line : {
                 "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f"_utf16,
                 "level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42001f"_utf16,
                 "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"_utf16,
                 "level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f"_utf16,
                 "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640032"_utf16,
             })
            codecs.append(codec("video/H264"_utf16, VIDEO_CLOCK_RATE, {}, fmtp_line));
        break;
    default:
        break;
    }
    return codecs;
}

static Vector<RTCRtpHeaderExtensionCapability> header_extensions()
{
    return {
        { .uri = "urn:ietf:params:rtp-hdrext:sdes:mid"_utf16 },
        { .uri = "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"_utf16 },
        { .uri = "urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id"_utf16 },
    };
}

// https://w3c.github.io/webrtc-pc/#dom-rtcrtpsender-getcapabilities
Optional<RTCRtpCapabilities> capabilities_for_sending(Utf16View kind)
{
    // The getCapabilities() method returns the most optimistic view of the capabilities of the system for sending
    // media of the given kind. If the system has no capabilities corresponding to the value of the kind argument,
    // getCapabilities returns null.
    // NB: What can be sent is what there is an encoder for, which is less than what can be received.
    Vector<RTCRtpCodec> codecs;
    if (kind == "audio"sv) {
        codecs.append(opus());
    } else if (kind == "video"sv) {
        for (auto codec_id : MediaCapture::VideoTrackEncoder::supported_codecs())
            codecs.extend(video_codecs(codec_id));
    } else {
        return {};
    }
    return RTCRtpCapabilities { .codecs = move(codecs), .header_extensions = header_extensions() };
}

// https://w3c.github.io/webrtc-pc/#dom-rtcrtpreceiver-getcapabilities
Optional<RTCRtpCapabilities> capabilities_for_receiving(Utf16View kind)
{
    // The getCapabilities() method returns the most optimistic view of the capabilities of the system for receiving
    // media of the given kind. If the system has no capabilities corresponding to the value of the kind argument,
    // getCapabilities returns null.
    Vector<RTCRtpCodec> codecs;
    if (kind == "audio"sv) {
        codecs.append(opus());
        codecs.append(codec("audio/PCMU"_utf16, 8000));
        codecs.append(codec("audio/PCMA"_utf16, 8000));
    } else if (kind == "video"sv) {
        for (auto codec_id : { Media::CodecID::VP8, Media::CodecID::VP9, Media::CodecID::H264 })
            codecs.extend(video_codecs(codec_id));
    } else {
        return {};
    }
    return RTCRtpCapabilities { .codecs = move(codecs), .header_extensions = header_extensions() };
}

}
