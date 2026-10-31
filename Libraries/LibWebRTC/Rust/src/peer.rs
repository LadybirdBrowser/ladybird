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

pub fn build_api(local_network: LocalNetwork) -> Result<API> {
    let mut media_engine = MediaEngine::default();
    media_engine.register_default_codecs()?;
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
