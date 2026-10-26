/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IPC dispatch loop for the WebRTCClient service.
//!
//! Reads `WebRTCClientServer` messages off a `libipc_rust` transport,
//! dispatches each to a handler that drives `webrtc-rs`, and emits
//! `WebRTCClientClient` events (state changes, on_track, ICE candidates,
//! per-request completion replies). Promise-shaped commands carry a
//! `request_id` that the host correlates with a stored promise on its end.

use std::collections::{HashMap, HashSet, VecDeque};
use std::future::Future;
use std::net::{IpAddr, Ipv4Addr};
use std::pin::Pin;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

use anyhow::{Result, anyhow};
use libipc_rust::EndpointMessage;
use tokio::sync::{Mutex as AsyncMutex, mpsc, oneshot, watch};
use webrtc::data_channel::RTCDataChannel;
use webrtc::data_channel::data_channel_init::RTCDataChannelInit;
use webrtc::ice::url::{ProtoType, SchemeType, Url};
use webrtc::ice_transport::ice_server::RTCIceServer;
use webrtc::peer_connection::RTCPeerConnection;
use webrtc::peer_connection::configuration::RTCConfiguration;
use webrtc::peer_connection::policy::ice_transport_policy::RTCIceTransportPolicy;
use webrtc::peer_connection::sdp::session_description::RTCSessionDescription;
use webrtc::track::track_local::track_local_static_sample::TrackLocalStaticSample;

use crate::peer::{LocalNetwork, build_api};

use crate::ipc::{
    ServerMessage as Message, Transport, client as Client, server as Server, to_optional_string, to_string,
};

type SharedWriter = Arc<Transport>;

// A page controls everything below, so these bound what one renderer can make its helper hold.
const MAX_PEER_CONNECTIONS: usize = 256;
const MAX_TRANSCEIVERS: usize = 1024;
const MAX_AUDIO_SENDERS: usize = 1024;
const MAX_DATA_CHANNELS: usize = 1024;
const MAX_ICE_SERVER_URLS: usize = 32;
// Data a channel may have waiting to go out. A page that sends more than this is closed rather than buffered.
const MAX_DATA_CHANNEL_BUFFERED_AMOUNT: usize = 16 * 1024 * 1024;
// Messages waiting for the renderer to read them. Past this, incoming media is dropped and data channels are closed,
// so a peer can't grow the helper's memory while the renderer is stalled.
const MAX_PENDING_OUTGOING_MESSAGES: usize = 2048;
// Messages from the renderer waiting to be handled. Past this, the renderer's sends wait instead.
const INCOMING_QUEUE_CAPACITY: usize = 256;
// Requests waiting behind a connection's host lookups. Past this, they're dropped.
const MAX_DEFERRED_REQUESTS: usize = 4 * INCOMING_QUEUE_CAPACITY;
const HOST_LOOKUP_TIMEOUT: Duration = Duration::from_secs(5);
const TEARDOWN_TIMEOUT: Duration = Duration::from_secs(2);

/// Whether the browser lets this helper use the network at all. It doesn't when a proxy is configured, since none of
/// WebRTC's traffic could go through it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NetworkAccess {
    Allowed,
    Denied,
}

struct AudioSender {
    pc_id: u64,
    sender: Arc<webrtc::rtp_transceiver::rtp_sender::RTCRtpSender>,
    // The host encodes (and possibly encrypts, as Discord's DAVE transform does) each frame, so its bytes go straight
    // onto the wire.
    track: Arc<TrackLocalStaticSample>,
}

#[derive(Default)]
struct Registry {
    // Every id the renderer has created a connection under and not closed yet, including those still waiting for
    // their host lookups, which aren't in pcs yet.
    pc_ids: HashSet<u64>,
    pcs: HashMap<u64, Arc<RTCPeerConnection>>,
    data_channels: HashMap<u64, Arc<RTCDataChannel>>,
    audio_senders: HashMap<u64, AudioSender>,
    channel_owners: HashMap<u64, u64>,
    transceivers: HashMap<u64, (u64, Arc<webrtc::rtp_transceiver::RTCRtpTransceiver>)>,
}

impl Registry {
    fn remove_data_channel(&mut self, channel_id: u64) -> Option<Arc<RTCDataChannel>> {
        self.channel_owners.remove(&channel_id);
        self.data_channels.remove(&channel_id)
    }
}

/// Host names the renderer is looking up for us, by request id.
#[derive(Default)]
struct HostLookups {
    next_request_id: AtomicU64,
    pending: std::sync::Mutex<HashMap<u64, oneshot::Sender<Vec<String>>>>,
}

impl HostLookups {
    fn pending(&self) -> std::sync::MutexGuard<'_, HashMap<u64, oneshot::Sender<Vec<String>>>> {
        self.pending.lock().unwrap_or_else(|error| error.into_inner())
    }

    fn start(&self) -> (u64, oneshot::Receiver<Vec<String>>) {
        let request_id = self.next_request_id.fetch_add(1, Ordering::Relaxed);
        let (sender, receiver) = oneshot::channel();
        self.pending().insert(request_id, sender);
        (request_id, receiver)
    }

    fn finish(&self, request_id: u64, addresses: Vec<String>) {
        if let Some(sender) = self.pending().remove(&request_id) {
            let _ = sender.send(addresses);
        }
    }

    fn forget(&self, request_id: u64) {
        self.pending().remove(&request_id);
    }

    fn cancel_all(&self) {
        self.pending().clear();
    }
}

struct State {
    network_access: NetworkAccess,
    registry: AsyncMutex<Registry>,
    writer: SharedWriter,
    next_remote_channel_id: AtomicU64,
    host_lookups: HostLookups,
}

impl State {
    fn new(writer: SharedWriter, network_access: NetworkAccess) -> Self {
        Self {
            network_access,
            registry: AsyncMutex::new(Registry::default()),
            writer,
            // Remote-opened channel ids start past the u32 range so they don't collide with
            // host-allocated ids on either side. The host uses the lower range.
            next_remote_channel_id: AtomicU64::new(1u64 << 32),
            host_lookups: HostLookups::default(),
        }
    }

    fn renderer_is_behind(&self) -> bool {
        self.writer.pending_message_count() > MAX_PENDING_OUTGOING_MESSAGES
    }
}

/// A request in the dispatch loop, or the creation of a connection whose request was already answered.
enum Deferred {
    Message(Message),
    Create(u64, PeerConfiguration),
}

async fn wait_for_disconnect(disconnected: &watch::Receiver<bool>) {
    let mut disconnected = disconnected.clone();
    let _ = disconnected.wait_for(|disconnected| *disconnected).await;
}

/// Run the dispatch loop on the given transport. Returns when the peer
/// disconnects or an unrecoverable error occurs.
pub async fn run(transport: Transport, network_access: NetworkAccess) -> Result<()> {
    let transport: SharedWriter = Arc::new(transport);
    let reader = transport.clone();
    let state = Arc::new(State::new(transport, network_access));

    let (message_sender, mut messages) = mpsc::channel::<Message>(INCOMING_QUEUE_CAPACITY);
    let (disconnect_sender, disconnected) = watch::channel(false);
    let weak_state = Arc::downgrade(&state);
    std::thread::spawn(move || {
        while let Some(message) = reader.receive() {
            let (bytes, attachments) = message.into_parts();
            let message = match Message::decode(bytes.as_ref(), attachments) {
                Ok(message) => message,
                Err(error) => {
                    log::warn!("refusing a message: {error:?}");
                    continue;
                }
            };
            // A handler in the dispatch loop may be waiting on this answer, so it can't wait in line behind it.
            if let Message::ResolvedHost(response) = message {
                if let Some(state) = weak_state.upgrade() {
                    let addresses = response.addresses.into_iter().map(to_string).collect();
                    state.host_lookups.finish(response.request_id, addresses);
                }
                continue;
            }
            if message_sender.blocking_send(message).is_err() {
                break;
            }
        }
        log::info!("peer disconnected");
        let _ = disconnect_sender.send(true);
    });

    // A connection whose ICE servers need host lookups waits for the renderer to answer them, so the loop keeps
    // reading meanwhile: the renderer may be blocked on a synchronous request of its own, which would hold up those
    // answers. The other requests wait in line behind the connection, so that everything still happens in the order
    // the renderer asked for it.
    let mut pending_creation: Option<Pin<Box<dyn Future<Output = Result<()>> + Send + '_>>> = None;
    let mut deferred: VecDeque<Deferred> = VecDeque::new();

    // Nothing is left to answer once the renderer is gone, so a handler that's stuck must not keep the helper alive.
    loop {
        let next = match pending_creation.is_none().then(|| deferred.pop_front()).flatten() {
            Some(next) => next,
            None => tokio::select! {
                biased;
                () = wait_for_disconnect(&disconnected) => break,
                result = async { pending_creation.as_mut().expect("a creation is pending").await }, if pending_creation.is_some() => {
                    pending_creation = None;
                    if let Err(error) = result {
                        log::warn!("handler error: {error:?}");
                    }
                    continue;
                },
                message = messages.recv() => match message {
                    Some(message) => Deferred::Message(message),
                    None => break,
                },
            },
        };

        let next = match next {
            // The renderer waits for the answer, so only the creation itself can wait in line.
            Deferred::Message(Message::CreatePeerConnection(req)) => {
                match begin_peer_connection(&state, req.pc_id, req.configuration).await {
                    Ok(Some(configuration)) => Deferred::Create(req.pc_id, configuration),
                    Ok(None) => continue,
                    Err(error) => {
                        log::warn!("handler error: {error:?}");
                        continue;
                    }
                }
            }
            next => next,
        };

        let renderer_waits = matches!(
            next,
            Deferred::Message(Message::InitTransport(_) | Message::GetSenderSsrc(_))
        );
        if pending_creation.is_some() && !renderer_waits {
            if deferred.len() >= MAX_DEFERRED_REQUESTS {
                log::warn!("dropping a request: too many are waiting for a peer connection's host lookups");
                continue;
            }
            deferred.push_back(next);
            continue;
        }

        let handled: Pin<Box<dyn Future<Output = Result<()>> + Send + '_>> = match next {
            Deferred::Create(pc_id, configuration) if needs_host_lookups(&configuration) => {
                pending_creation = Some(Box::pin(create_peer_connection(&state, pc_id, configuration)));
                continue;
            }
            Deferred::Create(pc_id, configuration) => Box::pin(create_peer_connection(&state, pc_id, configuration)),
            Deferred::Message(message) => Box::pin(dispatch(&state, message)),
        };
        tokio::select! {
            biased;
            () = wait_for_disconnect(&disconnected) => break,
            result = handled => if let Err(error) = result {
                log::warn!("handler error: {error:?}");
            },
        }
    }
    drop(pending_creation);

    state.host_lookups.cancel_all();
    let teardown = async {
        let peers = {
            let mut registry = state.registry.lock().await;
            registry.audio_senders.clear();
            registry.data_channels.clear();
            registry.channel_owners.clear();
            registry.transceivers.clear();
            std::mem::take(&mut registry.pcs)
        };
        for (_, peer) in peers {
            let _ = peer.close().await;
        }
    };
    if tokio::time::timeout(TEARDOWN_TIMEOUT, teardown).await.is_err() {
        log::warn!("gave up closing peer connections");
    }
    Ok(())
}

#[derive(serde::Deserialize)]
#[serde(rename_all = "kebab-case")]
enum HostCandidates {
    Exposed,
    Obfuscated,
}

#[derive(serde::Deserialize)]
struct HostConfiguration {
    ice_servers: Vec<RTCIceServer>,
    ice_transport_policy: String,
    bundle_policy: String,
    ice_candidate_pool_size: u8,
    host_candidates: HostCandidates,
}

struct PeerConfiguration {
    configuration: RTCConfiguration,
    local_network: LocalNetwork,
}

fn parse_configuration(json: &str, network_access: NetworkAccess) -> Result<PeerConfiguration> {
    use webrtc::peer_connection::policy::bundle_policy::RTCBundlePolicy;
    let host: HostConfiguration = serde_json::from_str(json)?;
    let mut configuration = RTCConfiguration {
        ice_servers: validate_ice_servers(host.ice_servers)?,
        ice_transport_policy: match host.ice_transport_policy.as_str() {
            "relay" => RTCIceTransportPolicy::Relay,
            "all" => RTCIceTransportPolicy::All,
            _ => return Err(anyhow!("Invalid ICE transport policy")),
        },
        bundle_policy: match host.bundle_policy.as_str() {
            "balanced" => RTCBundlePolicy::Balanced,
            "max-compat" => RTCBundlePolicy::MaxCompat,
            "max-bundle" => RTCBundlePolicy::MaxBundle,
            _ => return Err(anyhow!("Invalid bundle policy")),
        },
        ice_candidate_pool_size: host.ice_candidate_pool_size,
        ..Default::default()
    };
    let local_network = match (network_access, host.host_candidates) {
        // Relaying through no servers gathers no candidates, so ICE never enumerates interfaces or opens a socket.
        (NetworkAccess::Denied, _) => {
            configuration.ice_servers.clear();
            configuration.ice_transport_policy = RTCIceTransportPolicy::Relay;
            LocalNetwork::Unavailable
        }
        (NetworkAccess::Allowed, HostCandidates::Exposed) => LocalNetwork::Exposed,
        (NetworkAccess::Allowed, HostCandidates::Obfuscated) => LocalNetwork::Obfuscated,
    };
    Ok(PeerConfiguration {
        configuration,
        local_network,
    })
}

/// Keeps the ICE servers this helper can reach: STUN and TURN over UDP, which is all ICE gathers. Like Chromium, it
/// refuses ports below 1024 other than those of DNS, HTTP and HTTPS, so pages can't aim STUN and TURN traffic at
/// local services.
fn validate_ice_servers(servers: Vec<RTCIceServer>) -> Result<Vec<RTCIceServer>> {
    if servers.iter().map(|server| server.urls.len()).sum::<usize>() > MAX_ICE_SERVER_URLS {
        return Err(anyhow!("Too many ICE server URLs"));
    }
    let mut validated = Vec::new();
    for mut server in servers {
        let mut urls = Vec::new();
        for raw in &server.urls {
            let url = Url::parse_url(raw).map_err(|error| anyhow!("Invalid ICE server URL '{raw}': {error}"))?;
            let reachable = match url.scheme {
                SchemeType::Stun => true,
                SchemeType::Turn => url.proto == ProtoType::Udp,
                _ => false,
            };
            if !reachable {
                log::warn!("ignoring ICE server '{raw}': only STUN and TURN over UDP are supported");
                continue;
            }
            if url.port < 1024 && !matches!(url.port, 53 | 80 | 443) {
                log::warn!("ignoring ICE server '{raw}' on a restricted port");
                continue;
            }
            urls.push(raw.clone());
        }
        if !urls.is_empty() {
            server.urls = urls;
            validated.push(server);
        }
    }
    Ok(validated)
}

/// Looks host names up through the renderer, which asks RequestServer, so that the browser's DNS settings apply. The
/// dispatch loop waits for the answers, so the lookups all run at once under one deadline.
async fn look_up_hosts(state: &State, hosts: HashSet<String>) -> HashMap<String, Vec<Ipv4Addr>> {
    let mut lookups = Vec::new();
    for host in hosts {
        let (request_id, response) = state.host_lookups.start();
        let request = Client::ResolveHost {
            request_id,
            host: host.clone().into(),
        };
        if send(&state.writer, &request).is_err() {
            state.host_lookups.forget(request_id);
            continue;
        }
        lookups.push((host, request_id, response));
    }

    let deadline = tokio::time::Instant::now() + HOST_LOOKUP_TIMEOUT;
    let mut addresses_by_host = HashMap::new();
    for (host, request_id, response) in lookups {
        let Ok(Ok(addresses)) = tokio::time::timeout_at(deadline, response).await else {
            state.host_lookups.forget(request_id);
            log::warn!("looking up '{host}' failed");
            continue;
        };
        // ICE only gathers over IPv4.
        let addresses = addresses
            .iter()
            .filter_map(|address| match address.parse::<IpAddr>() {
                Ok(IpAddr::V4(address)) => Some(address),
                _ => None,
            })
            .collect();
        addresses_by_host.insert(host, addresses);
    }
    addresses_by_host
}

/// Replaces the host names in ICE server URLs with their addresses, since webrtc-rs would look them up with the
/// system's resolver.
async fn resolve_ice_servers(state: &State, servers: Vec<RTCIceServer>) -> Vec<RTCIceServer> {
    let hosts = servers
        .iter()
        .flat_map(|server| &server.urls)
        .filter_map(|raw| Url::parse_url(raw).ok())
        .filter(|url| url.host.parse::<IpAddr>().is_err())
        .map(|url| url.host)
        .collect();
    let addresses_by_host = look_up_hosts(state, hosts).await;

    let mut resolved = Vec::new();
    for mut server in servers {
        let mut urls = Vec::new();
        for raw in &server.urls {
            let Ok(mut url) = Url::parse_url(raw) else {
                continue;
            };
            match url.host.parse::<IpAddr>() {
                Ok(IpAddr::V4(_)) => urls.push(raw.clone()),
                Ok(IpAddr::V6(_)) => log::warn!("ignoring ICE server '{raw}': ICE only gathers over IPv4"),
                Err(_) => {
                    for address in addresses_by_host.get(&url.host).into_iter().flatten() {
                        url.host = address.to_string();
                        urls.push(url.to_string());
                    }
                }
            }
        }
        if !urls.is_empty() {
            server.urls = urls;
            resolved.push(server);
        }
    }
    resolved
}

/// Whether a remote candidate names its host, which webrtc-rs would look up with the system's resolver. mDNS names
/// are fine, since they're resolved on the local network.
fn candidate_needs_name_resolution(candidate: &str) -> bool {
    let candidate = candidate.strip_prefix("a=").unwrap_or(candidate);
    let Some(address) = candidate.split_ascii_whitespace().nth(4) else {
        return false;
    };
    address.parse::<IpAddr>().is_err() && !address.to_ascii_lowercase().ends_with(".local")
}

/// Drops the candidates in a remote description that only a DNS lookup could resolve.
fn remove_candidates_needing_name_resolution(sdp: &str) -> String {
    let mut result = String::with_capacity(sdp.len());
    for line in sdp.split_inclusive('\n') {
        if line.starts_with("a=candidate:") && candidate_needs_name_resolution(line.trim_end()) {
            log::debug!("ignoring a remote candidate with a host name: {}", line.trim_end());
            continue;
        }
        result.push_str(line);
    }
    result
}

/// Answers a request to create a peer connection, and returns what to create it with if it may be.
async fn begin_peer_connection(
    state: &State,
    pc_id: u64,
    configuration: libipc_rust::wire::Utf8String,
) -> Result<Option<PeerConfiguration>> {
    let parsed = {
        let mut registry = state.registry.lock().await;
        // A connection replaced under its id would never be closed, and would keep its resources and its events.
        if registry.pc_ids.contains(&pc_id) {
            Err(anyhow!("Peer connection id {pc_id} is already in use"))
        } else if registry.pc_ids.len() >= MAX_PEER_CONNECTIONS {
            Err(anyhow!("Too many peer connections"))
        } else {
            let parsed = parse_configuration(&to_string(configuration), state.network_access);
            if parsed.is_ok() {
                registry.pc_ids.insert(pc_id);
            }
            parsed
        }
    };
    let error = match &parsed {
        Ok(_) => String::new(),
        Err(error) => error.to_string(),
    };
    // The renderer waits for this answer, so it has to come before the host lookups the renderer serves.
    send(
        &state.writer,
        &Server::CreatePeerConnectionResponse { error: error.into() },
    )?;
    Ok(parsed.ok())
}

fn needs_host_lookups(configuration: &PeerConfiguration) -> bool {
    configuration.configuration.ice_servers.iter().any(|server| {
        server
            .urls
            .iter()
            .any(|raw| Url::parse_url(raw).is_ok_and(|url| url.host.parse::<IpAddr>().is_err()))
    })
}

async fn create_peer_connection(state: &Arc<State>, pc_id: u64, configuration: PeerConfiguration) -> Result<()> {
    let PeerConfiguration {
        mut configuration,
        local_network,
    } = configuration;
    configuration.ice_servers = resolve_ice_servers(state, std::mem::take(&mut configuration.ice_servers)).await;
    let created: Result<RTCPeerConnection> =
        async { Ok(build_api(local_network)?.new_peer_connection(configuration).await?) }.await;
    let pc = match created {
        Ok(pc) => Arc::new(pc),
        Err(error) => {
            // The renderer has already been told the connection exists, so it learns of this as a failure.
            log::warn!("creating peer connection {pc_id} failed: {error:#}");
            send(
                &state.writer,
                &Client::OnIceConnectionState {
                    pc_id,
                    state: "failed".into(),
                },
            )?;
            send(
                &state.writer,
                &Client::OnConnectionState {
                    pc_id,
                    state: "failed".into(),
                },
            )?;
            return Ok(());
        }
    };
    let mut registry = state.registry.lock().await;
    // The renderer may have closed the connection while its host lookups ran.
    if !registry.pc_ids.contains(&pc_id) {
        drop(registry);
        let _ = pc.close().await;
        return Ok(());
    }
    wire_pc_events(state, pc_id, &pc);
    registry.pcs.insert(pc_id, pc);
    Ok(())
}

async fn dispatch(state: &Arc<State>, message: Message) -> Result<()> {
    match message {
        Message::InitTransport(req) => {
            log::info!("InitTransport: peer_pid={}", req.peer_pid);
            let response = Server::InitTransportResponse {
                peer_pid: std::process::id() as i32,
            };
            send(&state.writer, &response)?;
        }
        Message::CreatePeerConnection(req) => {
            if let Some(configuration) = begin_peer_connection(state, req.pc_id, req.configuration).await? {
                create_peer_connection(state, req.pc_id, configuration).await?;
            }
        }
        Message::ClosePeerConnection(req) => {
            let pc = {
                let mut registry = state.registry.lock().await;
                registry.audio_senders.retain(|_, sender| sender.pc_id != req.pc_id);
                registry.transceivers.retain(|_, (owner, _)| *owner != req.pc_id);
                let channels: Vec<_> = registry
                    .channel_owners
                    .iter()
                    .filter_map(|(id, owner)| (*owner == req.pc_id).then_some(*id))
                    .collect();
                for id in channels {
                    registry.channel_owners.remove(&id);
                    registry.data_channels.remove(&id);
                }
                registry.pc_ids.remove(&req.pc_id);
                registry.pcs.remove(&req.pc_id)
            };
            if let Some(pc) = pc {
                let _ = pc.close().await;
            }
        }
        Message::GetSenderSsrc(req) => {
            let transceiver = state.registry.lock().await.transceivers.get(&req.sender_id).cloned();
            let ssrc = if let Some((_, transceiver)) = transceiver {
                transceiver
                    .sender()
                    .await
                    .get_parameters()
                    .await
                    .encodings
                    .first()
                    .map_or(0, |encoding| encoding.ssrc)
            } else {
                0
            };
            send(&state.writer, &Server::GetSenderSsrcResponse { ssrc })?;
        }
        Message::GetStats(req) => {
            let reports = async {
                let pc = find_pc(state, req.pc_id)
                    .await
                    .ok_or_else(|| anyhow!("Unknown peer connection"))?;
                let mut reports = serde_json::to_value(pc.get_stats().await)?;
                // webrtc-rs serializes epoch seconds; WebRTC DOM timestamps use milliseconds.
                if let Some(entries) = reports.as_object_mut() {
                    for entry in entries.values_mut() {
                        if let Some(timestamp) = entry.get("timestamp").and_then(serde_json::Value::as_f64) {
                            entry["timestamp"] = serde_json::json!(timestamp * 1000.0);
                        }
                    }
                }
                Ok::<_, anyhow::Error>(serde_json::to_string(&reports)?)
            }
            .await;
            // The renderer holds a promise for every request, so failures have to be answered too.
            let (reports, error) = match reports {
                Ok(reports) => (reports, String::new()),
                Err(error) => (String::new(), error.to_string()),
            };
            send(
                &state.writer,
                &Client::OnStats {
                    pc_id: req.pc_id,
                    request_id: req.request_id,
                    reports: reports.into(),
                    error: error.into(),
                },
            )?;
        }
        Message::CreateOffer(req) => {
            let event = match find_pc(state, req.pc_id).await {
                Some(pc) => match pc
                    .create_offer(Some(webrtc::peer_connection::offer_answer_options::RTCOfferOptions {
                        ice_restart: req.ice_restart,
                        ..Default::default()
                    }))
                    .await
                {
                    Ok(d) => Client::OnCreateOfferResult {
                        pc_id: req.pc_id,
                        request_id: req.request_id,
                        ok: true,
                        sdp_type: format!("{:?}", d.sdp_type).to_lowercase().into(),
                        sdp: d.sdp.into(),
                        error_kind: String::new().into(),
                        error_message: String::new().into(),
                    },
                    Err(e) => Client::OnCreateOfferResult {
                        pc_id: req.pc_id,
                        request_id: req.request_id,
                        ok: false,
                        sdp_type: String::new().into(),
                        sdp: String::new().into(),
                        error_kind: "OperationError".to_string().into(),
                        error_message: format!("{e}").into(),
                    },
                },
                None => Client::OnCreateOfferResult {
                    pc_id: req.pc_id,
                    request_id: req.request_id,
                    ok: false,
                    sdp_type: String::new().into(),
                    sdp: String::new().into(),
                    error_kind: "InvalidStateError".to_string().into(),
                    error_message: "unknown pc_id".to_string().into(),
                },
            };
            send(&state.writer, &event)?;
        }
        Message::CreateAnswer(req) => {
            let event = match find_pc(state, req.pc_id).await {
                Some(pc) => match pc.create_answer(None).await {
                    Ok(d) => Client::OnCreateAnswerResult {
                        pc_id: req.pc_id,
                        request_id: req.request_id,
                        ok: true,
                        sdp_type: format!("{:?}", d.sdp_type).to_lowercase().into(),
                        sdp: d.sdp.into(),
                        error_kind: String::new().into(),
                        error_message: String::new().into(),
                    },
                    Err(e) => Client::OnCreateAnswerResult {
                        pc_id: req.pc_id,
                        request_id: req.request_id,
                        ok: false,
                        sdp_type: String::new().into(),
                        sdp: String::new().into(),
                        error_kind: "OperationError".to_string().into(),
                        error_message: format!("{e}").into(),
                    },
                },
                None => Client::OnCreateAnswerResult {
                    pc_id: req.pc_id,
                    request_id: req.request_id,
                    ok: false,
                    sdp_type: String::new().into(),
                    sdp: String::new().into(),
                    error_kind: "InvalidStateError".to_string().into(),
                    error_message: "unknown pc_id".to_string().into(),
                },
            };
            send(&state.writer, &event)?;
        }
        Message::SetLocalDescription(req) => {
            let (ok, error_kind, error_message) =
                apply_description(state, req.pc_id, &to_string(req.sdp_type), to_string(req.sdp), true).await;
            send(
                &state.writer,
                &Client::OnSetLocalDescriptionResult {
                    pc_id: req.pc_id,
                    request_id: req.request_id,
                    ok,
                    error_kind: error_kind.into(),
                    error_message: error_message.into(),
                },
            )?;
        }
        Message::SetRemoteDescription(req) => {
            let (ok, error_kind, error_message) =
                apply_description(state, req.pc_id, &to_string(req.sdp_type), to_string(req.sdp), false).await;
            send(
                &state.writer,
                &Client::OnSetRemoteDescriptionResult {
                    pc_id: req.pc_id,
                    request_id: req.request_id,
                    ok,
                    error_kind: error_kind.into(),
                    error_message: error_message.into(),
                },
            )?;
        }
        Message::AddIceCandidate(req) => {
            let candidate = to_string(req.candidate);
            let event = if candidate_needs_name_resolution(&candidate) {
                log::debug!("ignoring a remote candidate with a host name: {candidate}");
                Client::OnAddIceCandidateResult {
                    pc_id: req.pc_id,
                    request_id: req.request_id,
                    ok: true,
                    error_kind: String::new().into(),
                    error_message: String::new().into(),
                }
            } else if let Some(pc) = find_pc(state, req.pc_id).await {
                let init = webrtc::ice_transport::ice_candidate::RTCIceCandidateInit {
                    candidate,
                    sdp_mid: to_optional_string(req.sdp_mid),
                    sdp_mline_index: req.sdp_mline_index.map(|v| v as u16),
                    username_fragment: to_optional_string(req.ufrag),
                };
                match pc.add_ice_candidate(init).await {
                    Ok(()) => Client::OnAddIceCandidateResult {
                        pc_id: req.pc_id,
                        request_id: req.request_id,
                        ok: true,
                        error_kind: String::new().into(),
                        error_message: String::new().into(),
                    },
                    Err(e) => Client::OnAddIceCandidateResult {
                        pc_id: req.pc_id,
                        request_id: req.request_id,
                        ok: false,
                        error_kind: "OperationError".to_string().into(),
                        error_message: format!("{e}").into(),
                    },
                }
            } else {
                Client::OnAddIceCandidateResult {
                    pc_id: req.pc_id,
                    request_id: req.request_id,
                    ok: false,
                    error_kind: "InvalidStateError".to_string().into(),
                    error_message: "unknown pc_id".to_string().into(),
                }
            };
            send(&state.writer, &event)?;
        }
        Message::AddTransceiver(req) => {
            if state.registry.lock().await.transceivers.len() >= MAX_TRANSCEIVERS {
                return Err(anyhow!("Too many transceivers"));
            }
            if let Some(pc) = find_pc(state, req.pc_id).await {
                use webrtc::rtp_transceiver::rtp_codec::RTPCodecType;
                use webrtc::rtp_transceiver::rtp_transceiver_direction::RTCRtpTransceiverDirection;
                let kind = match to_string(req.kind).as_str() {
                    "audio" => RTPCodecType::Audio,
                    "video" => RTPCodecType::Video,
                    other => {
                        log::warn!("AddTransceiver: unknown kind {other}");
                        return Ok(());
                    }
                };
                let direction = match to_string(req.direction).as_str() {
                    "sendrecv" => RTCRtpTransceiverDirection::Sendrecv,
                    "sendonly" => RTCRtpTransceiverDirection::Sendonly,
                    "recvonly" => RTCRtpTransceiverDirection::Recvonly,
                    "inactive" => RTCRtpTransceiverDirection::Inactive,
                    other => {
                        log::warn!("AddTransceiver: unknown direction {other}");
                        RTCRtpTransceiverDirection::Sendrecv
                    }
                };
                // Audio needs a placeholder encoding so a microphone can be attached
                // later. Video has no send pipeline; leave its sender empty. A dummy
                // VP8 track would otherwise be bound even for receive-only video,
                // rejecting an answer that selects another codec (e.g. H.264).
                let init = webrtc::rtp_transceiver::RTCRtpTransceiverInit {
                    direction: if kind == RTPCodecType::Audio {
                        RTCRtpTransceiverDirection::Sendrecv
                    } else {
                        RTCRtpTransceiverDirection::Recvonly
                    },
                    send_encodings: Vec::new(),
                };
                let transceiver = pc.add_transceiver_from_kind(kind, Some(init)).await?;
                transceiver.set_direction(direction).await;
                state
                    .registry
                    .lock()
                    .await
                    .transceivers
                    .insert(req.transceiver_id, (req.pc_id, transceiver));
            }
        }
        Message::SetTransceiverDirection(req) => {
            let transceiver = state
                .registry
                .lock()
                .await
                .transceivers
                .get(&req.transceiver_id)
                .cloned();
            if let Some((owner, transceiver)) = transceiver
                && owner == req.pc_id
            {
                use webrtc::rtp_transceiver::rtp_transceiver_direction::RTCRtpTransceiverDirection as D;
                let direction = match to_string(req.direction).as_str() {
                    "sendrecv" => D::Sendrecv,
                    "sendonly" => D::Sendonly,
                    "recvonly" => D::Recvonly,
                    "inactive" => D::Inactive,
                    _ => return Err(anyhow!("Invalid transceiver direction")),
                };
                transceiver.set_direction(direction).await;
            }
        }
        Message::StopTransceiver(req) => {
            let transceiver = state
                .registry
                .lock()
                .await
                .transceivers
                .get(&req.transceiver_id)
                .cloned();
            if let Some((owner, transceiver)) = transceiver
                && owner == req.pc_id
            {
                transceiver.stop().await?;
            }
        }
        Message::AddAudioTrack(req) => {
            {
                let registry = state.registry.lock().await;
                if registry.audio_senders.contains_key(&req.sender_id) {
                    return Ok(());
                }
                if registry.audio_senders.len() >= MAX_AUDIO_SENDERS {
                    return Err(anyhow!("Too many audio senders"));
                }
            }
            if let Some(pc) = find_pc(state, req.pc_id).await {
                use webrtc::api::media_engine::MIME_TYPE_OPUS;
                use webrtc::rtp_transceiver::rtp_codec::RTCRtpCodecCapability;
                let track = Arc::new(TrackLocalStaticSample::new(
                    RTCRtpCodecCapability {
                        mime_type: MIME_TYPE_OPUS.to_owned(),
                        clock_rate: 48000,
                        channels: 2,
                        sdp_fmtp_line: String::new(),
                        rtcp_feedback: vec![],
                    },
                    format!("audio-{}", req.sender_id),
                    format!("ladybird-stream-{}", req.pc_id),
                ));
                let existing = state.registry.lock().await.transceivers.get(&req.sender_id).cloned();
                let added = if let Some((owner, transceiver)) = existing {
                    if owner != req.pc_id {
                        return Err(anyhow!("Sender belongs to another peer"));
                    }
                    let sender = transceiver.sender().await;
                    sender.replace_track(Some(track.clone())).await.map(|()| sender)
                } else {
                    pc.add_track(track.clone()).await
                };
                match added {
                    Ok(rtp_sender) => {
                        for transceiver in pc.get_transceivers().await {
                            if Arc::ptr_eq(&transceiver.sender().await, &rtp_sender) {
                                state
                                    .registry
                                    .lock()
                                    .await
                                    .transceivers
                                    .insert(req.sender_id, (req.pc_id, transceiver));
                                break;
                            }
                        }
                        let params = rtp_sender.get_parameters().await;
                        let ssrc = params.encodings.first().map(|e| e.ssrc).unwrap_or(0);
                        // Negotiation can miss senders added after the initial offer/answer because their
                        // negotiated flag is not set in time. Bind now to initialize the packetizer;
                        // otherwise write_sample silently does nothing. Already-started senders are bound.
                        match rtp_sender.send(&params).await {
                            Ok(()) | Err(webrtc::Error::ErrRTPSenderSendAlreadyCalled) => {}
                            Err(e) => log::warn!("rtp_sender.send (forcing bind): {e}"),
                        }
                        // Reading RTCP drives feedback/statistics interceptors and prevents
                        // feedback from accumulating behind an unread sender stream.
                        let feedback_sender = rtp_sender.clone();
                        tokio::spawn(async move { while feedback_sender.read_rtcp().await.is_ok() {} });
                        state.registry.lock().await.audio_senders.insert(
                            req.sender_id,
                            AudioSender {
                                pc_id: req.pc_id,
                                sender: rtp_sender,
                                track,
                            },
                        );
                        log::info!(
                            "AddAudioTrack pc_id={} sender_id={} ssrc={}",
                            req.pc_id,
                            req.sender_id,
                            ssrc
                        );
                        send(
                            &state.writer,
                            &Client::OnAudioTrackAdded {
                                pc_id: req.pc_id,
                                sender_id: req.sender_id,
                                ssrc,
                            },
                        )?;
                    }
                    Err(e) => log::warn!("pc.add_track: {e}"),
                }
            }
        }
        Message::AudioTrackEncodedFrame(req) => {
            let track = state
                .registry
                .lock()
                .await
                .audio_senders
                .get(&req.sender_id)
                .map(|sender| sender.track.clone());
            if let Some(track) = track {
                use std::time::Duration;
                use webrtc::media::Sample;
                let sample = Sample {
                    data: req.payload.into(),
                    duration: Duration::from_micros(req.duration_micros as u64),
                    ..Default::default()
                };
                let len = sample.data.len();
                match track.write_sample(&sample).await {
                    Ok(()) => log::trace!(
                        "audio_track_encoded_frame sender_id={} duration_us={} len={len}",
                        req.sender_id,
                        req.duration_micros
                    ),
                    Err(e) => log::debug!("audio_track_encoded_frame write_sample failed: {e}"),
                }
            } else {
                log::debug!(
                    "audio_track_encoded_frame: no AudioSender for sender_id={}",
                    req.sender_id
                );
            }
        }
        Message::RemoveTrack(req) => {
            let removed = {
                let mut registry = state.registry.lock().await;
                registry.audio_senders.remove(&req.sender_id)
            };
            if let Some(sender) = removed
                && let Some(pc) = find_pc(state, req.pc_id).await
            {
                pc.remove_track(&sender.sender).await?;
            }
            log::info!("RemoveTrack pc_id={} sender_id={}", req.pc_id, req.sender_id);
        }
        Message::AddDataChannel(req) => {
            if state.registry.lock().await.data_channels.len() >= MAX_DATA_CHANNELS {
                send(
                    &state.writer,
                    &Client::OnDataChannelError {
                        channel_id: req.channel_id,
                        error: "Too many data channels".to_owned().into(),
                    },
                )?;
                send(
                    &state.writer,
                    &Client::OnDataChannelClosed {
                        channel_id: req.channel_id,
                    },
                )?;
                return Ok(());
            }
            if let Some(pc) = find_pc(state, req.pc_id).await {
                let init = RTCDataChannelInit {
                    ordered: Some(req.ordered),
                    max_packet_life_time: req.max_packet_life_time,
                    max_retransmits: req.max_retransmits,
                    protocol: Some(to_string(req.protocol)),
                    // webrtc-rs encodes negotiated-ness via a Some(channel_id); host already
                    // enforced that `negotiated == true` requires `id` to be set.
                    negotiated: if req.negotiated { req.id } else { None },
                };
                match pc.create_data_channel(&to_string(req.label), Some(init)).await {
                    Ok(dc) => {
                        wire_data_channel_events(state, req.channel_id, &dc).await;
                        {
                            let mut registry = state.registry.lock().await;
                            registry.channel_owners.insert(req.channel_id, req.pc_id);
                            registry.data_channels.insert(req.channel_id, dc);
                        }
                    }
                    Err(e) => log::warn!("create_data_channel: {e}"),
                }
            }
        }
        Message::DataChannelSendText(req) => {
            let data = to_string(req.data);
            if let Some(dc) = find_data_channel_with_room_for(state, req.channel_id, data.len()).await
                && let Err(e) = dc.send_text(data).await
            {
                log::warn!("data_channel_send_text: {e}");
            }
        }
        Message::DataChannelSendBinary(req) => {
            if let Some(dc) = find_data_channel_with_room_for(state, req.channel_id, req.data.len()).await
                && let Err(e) = dc.send(&bytes::Bytes::from(req.data)).await
            {
                log::warn!("data_channel_send_binary: {e}");
            }
        }
        Message::DataChannelClose(req) => {
            let removed = state.registry.lock().await.remove_data_channel(req.channel_id);
            if let Some(dc) = removed
                && let Err(e) = dc.close().await
            {
                log::warn!("data_channel_close: {e}");
            }
        }
        _ => log::warn!("refusing a response sent to the WebRTCClient service"),
    }
    Ok(())
}

async fn find_pc(state: &Arc<State>, pc_id: u64) -> Option<Arc<RTCPeerConnection>> {
    state.registry.lock().await.pcs.get(&pc_id).cloned()
}

async fn find_data_channel(state: &Arc<State>, channel_id: u64) -> Option<Arc<RTCDataChannel>> {
    state.registry.lock().await.data_channels.get(&channel_id).cloned()
}

/// Finds a channel that can take `length` more bytes. One that can't is closed, since the page is sending faster than
/// the peer accepts and its data would otherwise pile up here.
async fn find_data_channel_with_room_for(
    state: &Arc<State>,
    channel_id: u64,
    length: usize,
) -> Option<Arc<RTCDataChannel>> {
    let dc = find_data_channel(state, channel_id).await?;
    if dc.buffered_amount().await + length <= MAX_DATA_CHANNEL_BUFFERED_AMOUNT {
        return Some(dc);
    }
    fail_data_channel(state, channel_id, &dc, "Too much data is waiting to be sent").await;
    None
}

async fn fail_data_channel(state: &State, channel_id: u64, dc: &Arc<RTCDataChannel>, error: &str) {
    log::warn!("closing data channel {channel_id}: {error}");
    let _ = send(
        &state.writer,
        &Client::OnDataChannelError {
            channel_id,
            error: error.to_owned().into(),
        },
    );
    state.registry.lock().await.remove_data_channel(channel_id);
    let _ = dc.close().await;
}

async fn wire_data_channel_events(state: &Arc<State>, channel_id: u64, dc: &Arc<RTCDataChannel>) {
    let writer = state.writer.clone();
    dc.on_open(Box::new(move || {
        let writer = writer.clone();
        Box::pin(async move {
            let _ = send(&writer, &Client::OnDataChannelOpen { channel_id });
        })
    }));

    let writer = state.writer.clone();
    let weak_state = Arc::downgrade(state);
    dc.on_close(Box::new(move || {
        let writer = writer.clone();
        let weak_state = weak_state.clone();
        Box::pin(async move {
            if let Some(state) = weak_state.upgrade() {
                state.registry.lock().await.remove_data_channel(channel_id);
            }
            let _ = send(&writer, &Client::OnDataChannelClosed { channel_id });
        })
    }));

    let writer = state.writer.clone();
    dc.on_error(Box::new(move |err| {
        let writer = writer.clone();
        Box::pin(async move {
            let _ = send(
                &writer,
                &Client::OnDataChannelError {
                    channel_id,
                    error: format!("{err}").into(),
                },
            );
        })
    }));

    let writer = state.writer.clone();
    let weak_state = Arc::downgrade(state);
    let weak_dc = Arc::downgrade(dc);
    dc.on_message(Box::new(move |msg| {
        let writer = writer.clone();
        let weak_state = weak_state.clone();
        let weak_dc = weak_dc.clone();
        Box::pin(async move {
            if let Some(state) = weak_state.upgrade()
                && state.renderer_is_behind()
            {
                // Messages can't be dropped from a channel, so one that arrives faster than the renderer reads it is
                // closed instead. Closing waits for this handler, so it can't happen in here.
                if let Some(dc) = weak_dc.upgrade() {
                    tokio::spawn(async move {
                        fail_data_channel(
                            &state,
                            channel_id,
                            &dc,
                            "The page isn't keeping up with incoming messages",
                        )
                        .await;
                    });
                }
                return;
            }
            if msg.is_string {
                let text = String::from_utf8_lossy(&msg.data).into_owned();
                let _ = send(
                    &writer,
                    &Client::OnDataChannelMessageText {
                        channel_id,
                        data: text.into(),
                    },
                );
            } else {
                let _ = send(
                    &writer,
                    &Client::OnDataChannelMessageBinary {
                        channel_id,
                        data: msg.data.to_vec(),
                    },
                );
            }
        })
    }));

    let writer = state.writer.clone();
    dc.on_buffered_amount_low(Box::new(move || {
        let writer = writer.clone();
        Box::pin(async move {
            let _ = send(&writer, &Client::OnDataChannelBufferedAmountLow { channel_id });
        })
    }))
    .await;
}

async fn apply_description(
    state: &Arc<State>,
    pc_id: u64,
    sdp_type: &str,
    sdp: String,
    is_local: bool,
) -> (bool, String, String) {
    let Some(pc) = find_pc(state, pc_id).await else {
        return (false, "InvalidStateError".to_string(), "unknown pc_id".to_string());
    };
    // Discord's SDP marks every m= section as `a=inactive`, and webrtc-rs's
    // extract_ice_details skips inactive sections wholesale, so the SDP-embedded
    // candidates never make it into the ICE agent. Pull them out ourselves and
    // trickle them in after set_remote_description.
    let sdp = if is_local {
        sdp
    } else {
        remove_candidates_needing_name_resolution(&sdp)
    };
    let extracted_candidates = if !is_local {
        extract_candidate_lines(&sdp)
    } else {
        Vec::new()
    };

    log_sdp_summary(pc_id, if is_local { "local" } else { sdp_type }, &sdp);
    let parsed = parse_session_description(sdp_type, sdp);
    let Some(desc) = parsed else {
        return (
            false,
            "InvalidAccessError".to_string(),
            "could not parse SDP".to_string(),
        );
    };
    let result = if is_local {
        pc.set_local_description(desc).await
    } else {
        pc.set_remote_description(desc).await
    };
    match result {
        Ok(()) => {
            for candidate in extracted_candidates {
                match pc.add_ice_candidate(candidate).await {
                    Ok(()) => {}
                    Err(e) => log::warn!("trickle ICE candidate from SDP failed: {e}"),
                }
            }
            (true, String::new(), String::new())
        }
        Err(e) => (false, "OperationError".to_string(), format!("{e}")),
    }
}

fn log_sdp_summary(pc_id: u64, sdp_type: &str, sdp: &str) {
    let mut m_count = 0usize;
    let mut ssrc_count = 0usize;
    let mut extmaps: Vec<String> = Vec::new();
    let mut m_lines: Vec<String> = Vec::new();
    let mut directions: Vec<String> = Vec::new();
    let mut mids: Vec<String> = Vec::new();
    for line in sdp.lines() {
        let line = line.trim();
        if line.starts_with("m=") {
            m_count += 1;
            m_lines.push(line.to_string());
        } else if let Some(rest) = line.strip_prefix("a=") {
            if rest.starts_with("ssrc:") {
                ssrc_count += 1;
            } else if rest.starts_with("extmap:") {
                if !extmaps.contains(&rest.to_string()) {
                    extmaps.push(rest.to_string());
                }
            } else if rest == "sendrecv" || rest == "recvonly" || rest == "sendonly" || rest == "inactive" {
                directions.push(rest.to_string());
            } else if let Some(mid) = rest.strip_prefix("mid:") {
                mids.push(mid.to_string());
            }
        }
    }
    log::info!(
        "{sdp_type} SDP pc_id={pc_id} m_sections={m_count} ssrc_lines={ssrc_count} extmaps={} directions={:?} mids={:?}",
        extmaps.len(),
        directions,
        mids,
    );
    for ext in &extmaps {
        log::info!("  extmap: {ext}");
    }
    for m in m_lines.iter().take(3) {
        log::info!("  m=: {m}");
    }
}

fn extract_candidate_lines(sdp: &str) -> Vec<webrtc::ice_transport::ice_candidate::RTCIceCandidateInit> {
    use webrtc::ice_transport::ice_candidate::RTCIceCandidateInit;
    let mut out = Vec::new();
    let mut section: Vec<String> = Vec::new();
    let mut mid = None;
    let mut index: Option<u16> = None;
    let flush =
        |out: &mut Vec<RTCIceCandidateInit>, section: &mut Vec<String>, mid: &Option<String>, index: Option<u16>| {
            for candidate in section.drain(..) {
                out.push(RTCIceCandidateInit {
                    candidate,
                    sdp_mid: mid.clone(),
                    sdp_mline_index: index,
                    username_fragment: None,
                });
            }
        };
    for line in sdp.lines().map(str::trim) {
        if line.starts_with("m=") {
            flush(&mut out, &mut section, &mid, index);
            mid = None;
            index = Some(index.map_or(0, |i| i + 1));
        } else if let Some(value) = line.strip_prefix("a=mid:") {
            mid = Some(value.to_owned());
        } else if let Some(candidate) = line.strip_prefix("a=candidate:") {
            let candidate = format!("candidate:{candidate}");
            if !section.contains(&candidate) {
                section.push(candidate);
            }
        }
    }
    flush(&mut out, &mut section, &mid, index);
    out
}

fn parse_session_description(sdp_type: &str, sdp: String) -> Option<RTCSessionDescription> {
    let result = match sdp_type.to_ascii_lowercase().as_str() {
        "offer" => RTCSessionDescription::offer(sdp),
        "answer" => RTCSessionDescription::answer(sdp),
        "pranswer" => RTCSessionDescription::pranswer(sdp),
        "rollback" => {
            let mut description = RTCSessionDescription::default();
            description.sdp_type = webrtc::peer_connection::sdp::sdp_type::RTCSdpType::Rollback;
            Ok(description)
        }
        other => {
            log::warn!("unknown sdp type: {other}");
            return None;
        }
    };
    match result {
        Ok(d) => Some(d),
        Err(e) => {
            log::warn!("session description parse: {e}");
            None
        }
    }
}

fn wire_pc_events(state: &Arc<State>, pc_id: u64, pc: &Arc<RTCPeerConnection>) {
    let writer = state.writer.clone();
    pc.on_peer_connection_state_change(Box::new(move |s| {
        let writer = writer.clone();
        Box::pin(async move {
            let _ = send(
                &writer,
                &Client::OnConnectionState {
                    pc_id,
                    state: format!("{s:?}").to_lowercase().into(),
                },
            );
        })
    }));

    let writer = state.writer.clone();
    pc.on_signaling_state_change(Box::new(move |s| {
        let writer = writer.clone();
        Box::pin(async move {
            let _ = send(
                &writer,
                &Client::OnSignalingState {
                    pc_id,
                    state: format!("{s:?}").to_lowercase().into(),
                },
            );
        })
    }));

    let writer = state.writer.clone();
    pc.on_ice_connection_state_change(Box::new(move |s| {
        let writer = writer.clone();
        Box::pin(async move {
            let _ = send(
                &writer,
                &Client::OnIceConnectionState {
                    pc_id,
                    state: format!("{s:?}").to_lowercase().into(),
                },
            );
        })
    }));

    let writer = state.writer.clone();
    pc.on_ice_gathering_state_change(Box::new(move |s| {
        let writer = writer.clone();
        Box::pin(async move {
            let _ = send(
                &writer,
                &Client::OnIceGatheringState {
                    pc_id,
                    state: format!("{s:?}").to_lowercase().into(),
                },
            );
        })
    }));

    let writer = state.writer.clone();
    pc.on_ice_candidate(Box::new(move |c| {
        let writer = writer.clone();
        Box::pin(async move {
            let event = match c.and_then(|c| c.to_json().ok()) {
                Some(json) => Client::OnIceCandidate {
                    pc_id,
                    candidate: Some(json.candidate.into()),
                    sdp_mid: json.sdp_mid.map(Into::into),
                    sdp_mline_index: json.sdp_mline_index.map(u32::from),
                },
                None => Client::OnIceCandidate {
                    pc_id,
                    candidate: None,
                    sdp_mid: None,
                    sdp_mline_index: None,
                },
            };
            let _ = send(&writer, &event);
        })
    }));

    let writer = state.writer.clone();
    pc.on_negotiation_needed(Box::new(move || {
        let writer = writer.clone();
        Box::pin(async move {
            let _ = send(&writer, &Client::OnNegotiationNeeded { pc_id });
        })
    }));

    let writer = state.writer.clone();
    let weak_state = Arc::downgrade(state);
    pc.on_track(Box::new(move |track, receiver, transceiver| {
        let writer = writer.clone();
        let weak_state = weak_state.clone();
        Box::pin(async move {
            // FIXME: hand out monotonic ids and forward audio data through LibMedia plumbing
            //        rather than using ssrc-derived ids.
            let receiver_id = u64::from(track.ssrc());
            // The host's id for this transceiver, or 0 if the host never created it.
            let mut transceiver_id = 0;
            if let Some(state) = weak_state.upgrade() {
                let registry = state.registry.lock().await;
                if let Some((id, _)) = registry
                    .transceivers
                    .iter()
                    .find(|(_, (owner, registered))| *owner == pc_id && Arc::ptr_eq(registered, &transceiver))
                {
                    transceiver_id = *id;
                }
            }
            tokio::spawn(async move {
                while receiver.read_rtcp().await.is_ok() {}
            });
            let kind = format!("{}", track.kind());
            let stream_ids = [track.stream_id()];
            let track_mid = transceiver.mid();
            let track_rid = track.rid().to_string();
            log::info!(
                "on_track pc_id={pc_id} transceiver_id={transceiver_id} kind={kind} ssrc={} pt={} id={} stream_id={} mid={:?} rid={}",
                track.ssrc(),
                track.payload_type(),
                track.id(),
                track.stream_id(),
                track_mid,
                track_rid,
            );
            let _ = send(
                &writer,
                &Client::OnTrack {
                    pc_id,
                    receiver_id,
                    transceiver_id,
                    kind: kind.clone().into(),
                    stream_ids: stream_ids.iter().map(|id| id.as_str().into()).collect(),
                },
            );
            if kind == "audio" {
                let writer = writer.clone();
                tokio::spawn(async move {
                    let mut pkt_count: u64 = 0;
                    let mut ssrcs_seen: std::collections::HashSet<u32> = std::collections::HashSet::new();
                    loop {
                        match track.read_rtp().await {
                            Ok((packet, _)) => {
                                let ssrc = packet.header.ssrc;
                                let new_ssrc = ssrcs_seen.insert(ssrc);
                                if new_ssrc || pkt_count < 5 || pkt_count.is_multiple_of(200) {
                                    log::info!(
                                        "rtp pc_id={pc_id} track_ssrc={} pkt_ssrc={ssrc} pt={} seq={} ts={} ext={} ext_ids={:?} payload_len={} count={}",
                                        track.ssrc(),
                                        packet.header.payload_type,
                                        packet.header.sequence_number,
                                        packet.header.timestamp,
                                        packet.header.extension,
                                        packet.header.extensions.iter().map(|e| e.id).collect::<Vec<_>>(),
                                        packet.payload.len(),
                                        pkt_count,
                                    );
                                }
                                pkt_count += 1;
                                // Late audio is useless, so it's dropped while the renderer catches up.
                                if packet.payload.is_empty()
                                    || writer.pending_message_count() > MAX_PENDING_OUTGOING_MESSAGES
                                {
                                    continue;
                                }
                                let _ = send(
                                    &writer,
                                    &Client::OnEncodedAudioFrame {
                                        pc_id,
                                        receiver_id,
                                        ssrc,
                                        rtp_timestamp: packet.header.timestamp,
                                        sequence_number: packet.header.sequence_number,
                                        payload_type: packet.header.payload_type,
                                        payload: packet.payload.to_vec(),
                                    },
                                );
                            }
                            Err(e) => {
                                log::debug!("read_rtp ended pc_id={pc_id} track_ssrc={} after {pkt_count} pkts ssrcs={:?}: {e}", track.ssrc(), ssrcs_seen);
                                let _ = send(&writer, &Client::OnTrackEnded { pc_id, receiver_id });
                                break;
                            }
                        }
                    }
                });
            }
        })
    }));

    let state_for_dc = Arc::downgrade(state);
    pc.on_data_channel(Box::new(move |dc| {
        let state_for_dc = state_for_dc.clone();
        Box::pin(async move {
            let Some(state_for_dc) = state_for_dc.upgrade() else {
                return;
            };
            if state_for_dc.registry.lock().await.data_channels.len() >= MAX_DATA_CHANNELS {
                log::warn!("refusing a data channel from the remote peer: too many data channels");
                // Closing waits for this handler, so it can't happen in here.
                tokio::spawn(async move {
                    let _ = dc.close().await;
                });
                return;
            }
            let channel_id = state_for_dc.next_remote_channel_id.fetch_add(1, Ordering::Relaxed);

            // Snapshot channel parameters before registering callbacks (the `on_*`
            // closures move `dc` ownership into webrtc-rs internals).
            let label = dc.label().to_string();
            let protocol = dc.protocol().to_string();
            let ordered = dc.ordered();
            let max_packet_life_time = dc.max_packet_lifetime();
            let max_retransmits = dc.max_retransmits();
            let negotiated = dc.negotiated();
            let id = dc.id();

            let _ = send(
                &state_for_dc.writer,
                &Client::OnDataChannel {
                    pc_id,
                    channel_id,
                    label: label.into(),
                    ordered,
                    max_packet_life_time,
                    max_retransmits,
                    protocol: protocol.into(),
                    negotiated,
                    id: Some(id),
                },
            );

            wire_data_channel_events(&state_for_dc, channel_id, &dc).await;
            let mut registry = state_for_dc.registry.lock().await;
            registry.channel_owners.insert(channel_id, pc_id);
            registry.data_channels.insert(channel_id, dc);
        })
    }));
}

fn send<M: EndpointMessage>(writer: &SharedWriter, msg: &M) -> Result<()> {
    crate::ipc::send(writer, msg)
}

#[cfg(test)]
mod tests {
    use super::*;

    const CONFIG: &str = r#"{"ice_servers":[],"ice_transport_policy":"all","bundle_policy":"balanced","ice_candidate_pool_size":0,"host_candidates":"exposed"}"#;

    fn configuration_with_urls(urls: &[&str]) -> String {
        serde_json::json!({
            "ice_servers": [{ "urls": urls, "username": "user", "credential": "password" }],
            "ice_transport_policy": "all",
            "bundle_policy": "balanced",
            "ice_candidate_pool_size": 0,
            "host_candidates": "obfuscated",
        })
        .to_string()
    }

    // The renderer's end has to stay open for the state to send anything.
    fn new_state(network_access: NetworkAccess) -> (Arc<State>, Transport) {
        let (writer, renderer) = crate::ipc::create_paired_transports().unwrap();
        (Arc::new(State::new(Arc::new(writer), network_access)), renderer)
    }

    #[test]
    fn forwards_ice_configuration() {
        let PeerConfiguration {
            configuration: config,
            local_network,
        } = parse_configuration(r#"{"ice_servers":[{"urls":["turn:localhost:3478"],"username":"user","credential":"password"}],"ice_transport_policy":"relay","bundle_policy":"max-bundle","ice_candidate_pool_size":2,"host_candidates":"obfuscated"}"#, NetworkAccess::Allowed).unwrap();
        assert_eq!(local_network, LocalNetwork::Obfuscated);
        assert_eq!(config.ice_servers[0].urls, ["turn:localhost:3478"]);
        assert_eq!(config.ice_servers[0].username, "user");
        assert_eq!(config.ice_servers[0].credential, "password");
        assert_eq!(
            config.ice_transport_policy,
            webrtc::peer_connection::policy::ice_transport_policy::RTCIceTransportPolicy::Relay
        );
        assert_eq!(
            config.bundle_policy,
            webrtc::peer_connection::policy::bundle_policy::RTCBundlePolicy::MaxBundle
        );
        assert_eq!(config.ice_candidate_pool_size, 2);
    }

    #[test]
    fn keeps_only_reachable_ice_servers() {
        let urls = [
            "stun:192.0.2.1:3478",
            "stun:192.0.2.1:22",
            "turn:192.0.2.1:443?transport=udp",
            "turn:192.0.2.1:3478?transport=tcp",
            "turns:192.0.2.1:5349",
            "stun:192.0.2.1:53",
        ];
        let parsed = parse_configuration(&configuration_with_urls(&urls), NetworkAccess::Allowed).unwrap();
        assert_eq!(
            parsed.configuration.ice_servers[0].urls,
            [
                "stun:192.0.2.1:3478",
                "turn:192.0.2.1:443?transport=udp",
                "stun:192.0.2.1:53"
            ]
        );
        assert!(
            parse_configuration(
                &configuration_with_urls(&["http://example.com"]),
                NetworkAccess::Allowed
            )
            .is_err()
        );
        let too_many = vec!["stun:192.0.2.1:3478"; MAX_ICE_SERVER_URLS + 1];
        assert!(parse_configuration(&configuration_with_urls(&too_many), NetworkAccess::Allowed).is_err());
    }

    #[test]
    fn a_helper_without_network_access_gathers_nothing() {
        let parsed = parse_configuration(
            &configuration_with_urls(&["stun:192.0.2.1:3478"]),
            NetworkAccess::Denied,
        )
        .unwrap();
        assert!(parsed.configuration.ice_servers.is_empty());
        assert_eq!(parsed.configuration.ice_transport_policy, RTCIceTransportPolicy::Relay);
        assert_eq!(parsed.local_network, LocalNetwork::Unavailable);
    }

    #[test]
    fn candidates_with_host_names_are_dropped() {
        assert!(candidate_needs_name_resolution(
            "candidate:1 1 udp 2130706431 media.example.com 3478 typ host"
        ));
        assert!(!candidate_needs_name_resolution(
            "candidate:1 1 udp 2130706431 192.0.2.1 3478 typ host"
        ));
        assert!(!candidate_needs_name_resolution(
            "candidate:1 1 udp 2130706431 0d4d6f1c-2a3b-4c5d-8e9f-0a1b2c3d4e5f.local 3478 typ host"
        ));
        let sdp = "v=0\r\na=candidate:1 1 udp 1 media.example.com 3478 typ host\r\na=candidate:2 1 udp 1 192.0.2.1 3478 typ host\r\n";
        assert_eq!(
            remove_candidates_needing_name_resolution(sdp),
            "v=0\r\na=candidate:2 1 udp 1 192.0.2.1 3478 typ host\r\n"
        );
    }

    #[tokio::test]
    async fn ice_server_names_are_looked_up_through_the_renderer() {
        use crate::ipc::ClientMessage;
        let (writer, renderer) = crate::ipc::create_paired_transports().unwrap();
        let state = Arc::new(State::new(Arc::new(writer), NetworkAccess::Allowed));
        let lookups = state.clone();
        let renderer = std::thread::spawn(move || {
            let (bytes, attachments) = renderer.receive().unwrap().into_parts();
            let ClientMessage::ResolveHost(request) = ClientMessage::decode(bytes.as_ref(), attachments).unwrap()
            else {
                panic!("expected a host lookup");
            };
            assert_eq!(to_string(request.host), "stun.example");
            lookups.host_lookups.finish(
                request.request_id,
                vec!["192.0.2.1".to_owned(), "2001:db8::1".to_owned()],
            );
        });
        let servers = resolve_ice_servers(
            &state,
            vec![RTCIceServer {
                urls: vec![
                    "stun:stun.example:3478".to_owned(),
                    "stun:198.51.100.1:3478".to_owned(),
                    "stun:stun.example:3479".to_owned(),
                ],
                ..Default::default()
            }],
        )
        .await;
        renderer.join().unwrap();
        // The renderer answers a single lookup, so both URLs naming the host share it.
        assert_eq!(
            servers[0].urls,
            ["stun:192.0.2.1:3478", "stun:198.51.100.1:3478", "stun:192.0.2.1:3479"]
        );
    }

    fn post<M: EndpointMessage>(renderer: &Transport, message: &M) {
        let (bytes, attachments) = crate::ipc::encode(message).unwrap();
        renderer.post_message(bytes, attachments).unwrap();
    }

    /// Waits for the service's next message to the renderer, which may be a client event or a synchronous answer.
    fn receive(renderer: &Transport) -> Result<crate::ipc::ClientMessage, Message> {
        let (bytes, attachments) = renderer.receive().expect("the service hung up").into_parts();
        // None of the messages these tests expect carry attachments.
        match crate::ipc::ClientMessage::decode(bytes.as_ref(), Vec::new()) {
            Ok(message) => Ok(message),
            Err(_) => Err(Message::decode(bytes.as_ref(), attachments).expect("an answer from the service")),
        }
    }

    #[tokio::test(flavor = "multi_thread")]
    async fn synchronous_requests_do_not_wait_for_host_lookups() {
        use crate::ipc::ClientMessage;
        let (service, renderer) = crate::ipc::create_paired_transports().unwrap();
        let service = tokio::spawn(run(service, NetworkAccess::Allowed));
        let renderer = Arc::new(renderer);

        let elapsed = tokio::task::spawn_blocking({
            let renderer = renderer.clone();
            move || {
                post(
                    &renderer,
                    &Server::CreatePeerConnection {
                        pc_id: 1,
                        configuration: configuration_with_urls(&["stun:stun.example:3478"]).into(),
                    },
                );
                assert!(matches!(
                    receive(&renderer),
                    Err(Message::CreatePeerConnectionResponse(_))
                ));
                assert!(matches!(receive(&renderer), Ok(ClientMessage::ResolveHost(_))));

                // The renderer leaves the lookup unanswered while it waits for an answer of its own, as it does in
                // RTCRtpSender::get_parameters().
                let started = std::time::Instant::now();
                post(&renderer, &Server::GetSenderSsrc { sender_id: 1 });
                let Err(Message::GetSenderSsrcResponse(response)) = receive(&renderer) else {
                    panic!("expected the sender's SSRC");
                };
                assert_eq!(response.ssrc, 0);
                started.elapsed()
            }
        })
        .await
        .unwrap();
        assert!(
            elapsed < HOST_LOOKUP_TIMEOUT / 2,
            "the answer waited {elapsed:?} for the host lookups"
        );

        drop(renderer);
        tokio::time::timeout(Duration::from_secs(10), service)
            .await
            .expect("the service outlived its renderer")
            .unwrap()
            .unwrap();
    }

    #[tokio::test]
    async fn a_peer_connection_id_cannot_be_reused() {
        let (state, _renderer) = new_state(NetworkAccess::Allowed);
        for _ in 0..2 {
            command(
                &state,
                Server::CreatePeerConnection {
                    pc_id: 1,
                    configuration: CONFIG.to_owned().into(),
                },
            )
            .await;
        }
        let first = find_pc(&state, 1).await.unwrap();
        assert_eq!(state.registry.lock().await.pcs.len(), 1);
        command(&state, Server::ClosePeerConnection { pc_id: 1 }).await;
        assert!(state.registry.lock().await.pc_ids.is_empty());
        command(
            &state,
            Server::CreatePeerConnection {
                pc_id: 1,
                configuration: CONFIG.to_owned().into(),
            },
        )
        .await;
        assert!(!Arc::ptr_eq(&first, &find_pc(&state, 1).await.unwrap()));
        command(&state, Server::ClosePeerConnection { pc_id: 1 }).await;
    }

    #[test]
    fn candidates_keep_their_media_section() {
        let candidates = extract_candidate_lines(
            "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=candidate:one\r\na=mid:audio\r\na=candidate:one\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:video\r\na=candidate:two\r\n",
        );
        assert_eq!(candidates.len(), 2);
        assert_eq!(candidates[0].sdp_mid.as_deref(), Some("audio"));
        assert_eq!(candidates[0].sdp_mline_index, Some(0));
        assert_eq!(candidates[1].sdp_mid.as_deref(), Some("video"));
        assert_eq!(candidates[1].sdp_mline_index, Some(1));
    }

    #[test]
    fn rollback_has_the_correct_type() {
        let description = parse_session_description("rollback", String::new()).unwrap();
        assert_eq!(
            description.sdp_type,
            webrtc::peer_connection::sdp::sdp_type::RTCSdpType::Rollback
        );
    }

    async fn command<M: EndpointMessage>(state: &Arc<State>, message: M) {
        let (bytes, attachments) = crate::ipc::encode(&message).unwrap();
        let message = Message::decode(&bytes, attachments).unwrap();
        tokio::time::timeout(std::time::Duration::from_secs(5), dispatch(state, message))
            .await
            .expect("IPC command deadlocked")
            .unwrap();
    }

    #[tokio::test]
    async fn receive_only_video_accepts_h264_only_answer() {
        use webrtc::api::{APIBuilder, media_engine::MediaEngine};
        use webrtc::rtp_transceiver::rtp_codec::{RTCRtpCodecCapability, RTCRtpCodecParameters, RTPCodecType};
        let (state, _renderer) = new_state(NetworkAccess::Allowed);
        command(
            &state,
            Server::CreatePeerConnection {
                pc_id: 1,
                configuration: CONFIG.to_owned().into(),
            },
        )
        .await;
        command(
            &state,
            Server::AddTransceiver {
                pc_id: 1,
                transceiver_id: 1,
                kind: "video".to_owned().into(),
                direction: "recvonly".to_owned().into(),
            },
        )
        .await;
        let local = find_pc(&state, 1).await.unwrap();
        // Discord selects H.264 even when no video is being sent.
        let mut media = MediaEngine::default();
        media
            .register_codec(
                RTCRtpCodecParameters {
                    capability: RTCRtpCodecCapability {
                        mime_type: "video/H264".to_owned(),
                        clock_rate: 90000,
                        sdp_fmtp_line: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f"
                            .to_owned(),
                        ..Default::default()
                    },
                    payload_type: 102,
                    ..Default::default()
                },
                RTPCodecType::Video,
            )
            .unwrap();
        let remote = APIBuilder::new()
            .with_media_engine(media)
            .build()
            .new_peer_connection(RTCConfiguration::default())
            .await
            .unwrap();
        let offer = local.create_offer(None).await.unwrap();
        local.set_local_description(offer.clone()).await.unwrap();
        remote.set_remote_description(offer).await.unwrap();
        let answer = remote.create_answer(None).await.unwrap();
        assert!(answer.sdp.contains("H264/90000"));
        assert!(!answer.sdp.contains("VP8/90000"));
        remote.set_local_description(answer.clone()).await.unwrap();
        let result = local.set_remote_description(answer).await;
        local.close().await.unwrap();
        remote.close().await.unwrap();
        result.expect("receive-only video must not try to bind a placeholder VP8 sender");
    }

    #[tokio::test]
    async fn closing_peer_releases_only_its_resources() {
        let (state, _renderer) = new_state(NetworkAccess::Allowed);
        for pc_id in [1, 2] {
            command(
                &state,
                Server::CreatePeerConnection {
                    pc_id,
                    configuration: CONFIG.to_owned().into(),
                },
            )
            .await;
            if pc_id == 1 {
                command(
                    &state,
                    Server::AddTransceiver {
                        pc_id,
                        transceiver_id: pc_id,
                        kind: "audio".to_owned().into(),
                        direction: "recvonly".to_owned().into(),
                    },
                )
                .await;
            }
            command(
                &state,
                Server::AddAudioTrack {
                    pc_id,
                    sender_id: pc_id,
                },
            )
            .await;
            command(
                &state,
                Server::AddAudioTrack {
                    pc_id,
                    sender_id: pc_id,
                },
            )
            .await;
            let pc = find_pc(&state, pc_id).await.unwrap();
            assert_eq!(
                pc.get_transceivers().await.len(),
                1,
                "attaching a track must reuse its transceiver"
            );
            command(
                &state,
                Server::AddDataChannel {
                    pc_id,
                    channel_id: pc_id,
                    label: "test".to_owned().into(),
                    ordered: true,
                    max_packet_life_time: None,
                    max_retransmits: None,
                    protocol: String::new().into(),
                    negotiated: false,
                    id: None,
                },
            )
            .await;
        }
        command(&state, Server::ClosePeerConnection { pc_id: 1 }).await;
        command(&state, Server::ClosePeerConnection { pc_id: 1 }).await;
        {
            let registry = state.registry.lock().await;
            assert!(!registry.pcs.contains_key(&1));
            assert!(!registry.audio_senders.contains_key(&1));
            assert!(!registry.data_channels.contains_key(&1));
            assert!(registry.pcs.contains_key(&2));
            assert!(registry.audio_senders.contains_key(&2));
            assert!(registry.data_channels.contains_key(&2));
        }
        command(&state, Server::ClosePeerConnection { pc_id: 2 }).await;
        assert!(state.registry.lock().await.channel_owners.is_empty());
        assert_eq!(
            Arc::strong_count(&state),
            1,
            "peer callbacks must not retain the service"
        );
    }
}
