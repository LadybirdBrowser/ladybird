/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PeerConnection helpers built on top of the `webrtc` crate.

use std::sync::Arc;

use anyhow::Result;
use webrtc::api::API;
use webrtc::api::APIBuilder;
use webrtc::api::media_engine::MediaEngine;
use webrtc::api::setting_engine::SettingEngine;
use webrtc::ice::mdns::MulticastDnsMode;
use webrtc::ice::network_type::NetworkType;
use webrtc::peer_connection::RTCPeerConnection;
use webrtc::peer_connection::configuration::RTCConfiguration;
use webrtc::rtp_transceiver::rtp_codec::{RTCRtpHeaderExtensionCapability, RTPCodecType};

/// What a peer connection's ICE agent may reveal about, and do on, the local network.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LocalNetwork {
    /// Host candidates carry the interfaces' addresses.
    Exposed,
    /// Host candidates carry random mDNS names instead, so pages can't learn local addresses.
    /// https://datatracker.ietf.org/doc/html/draft-ietf-mmusic-mdns-ice-candidates
    Obfuscated,
    /// Nothing touches the network, for a helper that has no network access.
    Unavailable,
}

pub fn build_default_api() -> Result<API> {
    build_api(LocalNetwork::Exposed)
}

/// Offers the audio codecs that the renderer can decode, which are those that webrtc-rs does by default except for G722,
/// and the video codecs whose frames [`crate::video::FrameAssembler`] can put together for the renderer to decode.
fn register_codecs(media_engine: &mut MediaEngine) -> Result<()> {
    use webrtc::api::media_engine::{
        MIME_TYPE_H264, MIME_TYPE_OPUS, MIME_TYPE_PCMA, MIME_TYPE_PCMU, MIME_TYPE_VP8, MIME_TYPE_VP9,
    };
    use webrtc::rtp_transceiver::RTCPFeedback;
    use webrtc::rtp_transceiver::rtp_codec::{RTCRtpCodecCapability, RTCRtpCodecParameters};

    let audio_codecs = [
        (MIME_TYPE_OPUS, 48000, 2, "minptime=10;useinbandfec=1", 111),
        (MIME_TYPE_PCMU, 8000, 0, "", 0),
        (MIME_TYPE_PCMA, 8000, 0, "", 8),
    ];
    for (mime_type, clock_rate, channels, sdp_fmtp_line, payload_type) in audio_codecs {
        media_engine.register_codec(
            RTCRtpCodecParameters {
                capability: RTCRtpCodecCapability {
                    mime_type: mime_type.to_owned(),
                    clock_rate,
                    channels,
                    sdp_fmtp_line: sdp_fmtp_line.to_owned(),
                    rtcp_feedback: vec![],
                },
                payload_type,
                ..Default::default()
            },
            RTPCodecType::Audio,
        )?;
    }

    let video_rtcp_feedback: Vec<RTCPFeedback> = [("goog-remb", ""), ("ccm", "fir"), ("nack", ""), ("nack", "pli")]
        .into_iter()
        .map(|(typ, parameter)| RTCPFeedback {
            typ: typ.to_owned(),
            parameter: parameter.to_owned(),
        })
        .collect();
    let video_codecs = [
        (MIME_TYPE_VP8, "", 96),
        (MIME_TYPE_VP9, "profile-id=0", 98),
        (
            MIME_TYPE_H264,
            "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f",
            102,
        ),
        (
            MIME_TYPE_H264,
            "level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42001f",
            127,
        ),
        (
            MIME_TYPE_H264,
            "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
            125,
        ),
        (
            MIME_TYPE_H264,
            "level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f",
            108,
        ),
        (
            MIME_TYPE_H264,
            "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640032",
            123,
        ),
    ];
    for (mime_type, sdp_fmtp_line, payload_type) in video_codecs {
        media_engine.register_codec(
            RTCRtpCodecParameters {
                capability: RTCRtpCodecCapability {
                    mime_type: mime_type.to_owned(),
                    clock_rate: 90000,
                    channels: 0,
                    sdp_fmtp_line: sdp_fmtp_line.to_owned(),
                    rtcp_feedback: video_rtcp_feedback.clone(),
                },
                payload_type,
                ..Default::default()
            },
            RTPCodecType::Video,
        )?;
    }
    Ok(())
}

pub fn build_api(local_network: LocalNetwork) -> Result<API> {
    let mut media_engine = MediaEngine::default();
    register_codecs(&mut media_engine)?;
    // Required for routing bundled RTP streams to the right transceiver — without these
    // webrtc-rs falls back to "needs simulcast probing" and drops every speech packet.
    for uri in [
        "urn:ietf:params:rtp-hdrext:sdes:mid",
        "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id",
        "urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id",
    ] {
        media_engine.register_header_extension(
            RTCRtpHeaderExtensionCapability { uri: uri.to_owned() },
            RTPCodecType::Audio,
            None,
        )?;
        media_engine.register_header_extension(
            RTCRtpHeaderExtensionCapability { uri: uri.to_owned() },
            RTPCodecType::Video,
            None,
        )?;
    }
    let registry = webrtc::api::interceptor_registry::register_default_interceptors(
        webrtc::interceptor::registry::Registry::new(),
        &mut media_engine,
    )?;
    let mut setting_engine = SettingEngine::default();
    // Restrict ICE to IPv4. Link-local IPv6 binds need a scope id that webrtc-rs
    // doesn't pass through, so any v6 host candidate fails to bind and we end up
    // with no candidate pairs at all.
    setting_engine.set_network_types(vec![NetworkType::Udp4]);
    // Remote mDNS candidates are always resolved; local ones only replace the addresses when asked to. Peers that
    // can't resolve them still learn this side's address from its connectivity checks.
    setting_engine.set_ice_multicast_dns_mode(match local_network {
        LocalNetwork::Exposed => MulticastDnsMode::QueryOnly,
        LocalNetwork::Obfuscated => MulticastDnsMode::QueryAndGather,
        LocalNetwork::Unavailable => MulticastDnsMode::Disabled,
    });
    Ok(APIBuilder::new()
        .with_media_engine(media_engine)
        .with_interceptor_registry(registry)
        .with_setting_engine(setting_engine)
        .build())
}

pub async fn new_peer_connection(api: &API, config: RTCConfiguration) -> Result<Arc<RTCPeerConnection>> {
    let pc = api.new_peer_connection(config).await?;
    Ok(Arc::new(pc))
}
