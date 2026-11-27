/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A stub resolver that never waits or performs I/O. The caller supplies lookup requests, socket events, system
//! resolver results and clock ticks. `take_action()` returns work for the caller; `next_deadline()` gives the next tick time.

mod cache;

#[cfg(test)]
mod tests;

pub use cache::{LookupResult, SecurityStatus};

use crate::dnssec::{self, Fetches, Step, ValidationResult, Validator};
use crate::message::{
    Class, DomainName, Message, OpCode, Opt, Options, Question, RecordData, ResourceRecord, ResourceType, ResponseCode,
};
use crate::transport::{self, Connection};
use cache::{Cache, CacheEntry};
use std::collections::{HashMap, VecDeque};
use std::fmt;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

pub type LookupId = u64;
pub type ConnectionId = u64;
pub type SystemRequestId = u64;

const QUERY_RETRY_INTERVAL: Duration = Duration::from_secs(1);
const QUERY_MAX_ATTEMPTS: usize = 5;
/// How long a connection that was replaced for answering nothing still takes late answers.
const REPLACED_CONNECTION_DRAIN_TIME: Duration = Duration::from_secs(5);
const MAX_CACHE_ENTRIES: usize = 4096;
// How long connecting to the server may take, including looking up its name and any TLS handshake.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
const IPV6_REACHABILITY_CHECK_INTERVAL: Duration = Duration::from_secs(1);
const SYSTEM_RESOLVER_SYNTHETIC_TTL_SECONDS: u32 = 60;
// How long the system resolver may take to answer for a family, after which it counts as having failed. Its answer can
// get lost, such as in another process that goes away. The UI process, which looks names up for RequestServer, skips a
// lookup that waited this long for a worker.
const SYSTEM_RESOLVER_TIMEOUT: Duration = Duration::from_secs(15);
// RFC 8305, 3. Hostname Resolution Query Handling: the Resolution Delay.
const IPV6_RESOLUTION_DELAY: Duration = Duration::from_millis(50);
// curl never learns of IPv4 records that come after it went ahead with IPv6 records that turn out to be unreachable.
const IPV4_RESOLUTION_DELAY: Duration = Duration::from_millis(1000);

/// Why a lookup failed. The message never changes, so it can outlive the resolver; the detail, if any, says more.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error {
    pub message: &'static str,
    pub detail: Option<String>,
}

impl Error {
    const fn new(message: &'static str) -> Self {
        Self { message, detail: None }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.detail {
            Some(detail) => write!(f, "{}: {detail}", self.message),
            None => f.write_str(self.message),
        }
    }
}

impl From<transport::Error> for Error {
    fn from(error: transport::Error) -> Self {
        match error {
            transport::Error::TimedOut => Self::new("Connecting to the DNS server timed out"),
            transport::Error::Closed => Self::new("The DNS server closed the connection"),
            transport::Error::Socket(detail) => Self {
                message: "DNS server connection failed",
                detail: Some(detail),
            },
            transport::Error::FrameTooLarge => Self::new("DNS message is too large for a TCP frame"),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ServerTransport {
    /// UDP, retried over TCP when a response comes back truncated.
    Udp,
    /// RFC 7858 DNS over TLS.
    Tls,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ServerConfig {
    /// A host name, which the system resolver looks up, or an address.
    pub host: String,
    pub port: u16,
    pub transport: ServerTransport,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConnectKind {
    Udp,
    Tcp,
    /// A TCP connection that counts as connected once its TLS handshake with `server_name` is done.
    Tls,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AddressFamily {
    V4,
    V6,
}

#[derive(Debug)]
pub enum Action {
    Connect {
        connection: ConnectionId,
        kind: ConnectKind,
        address: SocketAddr,
        server_name: String,
    },
    Write {
        connection: ConnectionId,
        bytes: Vec<u8>,
    },
    Close {
        connection: ConnectionId,
    },
    SystemResolve {
        request: SystemRequestId,
        name: String,
        family: AddressFamily,
    },
    /// The answer to a `SystemResolve` action is no longer wanted, and it need not be looked up.
    CancelSystemResolve {
        request: SystemRequestId,
    },
    LookupDone {
        lookup: LookupId,
        result: Result<Arc<LookupResult>, Error>,
    },
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LookupOptions {
    pub validate_dnssec_locally: bool,
    /// Asks the system resolver for AAAA records even when no global IPv6 route exists, as a host on the local
    /// network may still be reachable over IPv6.
    pub query_aaaa_without_global_ipv6: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum LinkRole {
    Primary,
    // Only in UDP mode: where questions whose answers came back truncated are asked again.
    TcpFallback,
}

struct Link {
    id: ConnectionId,
    role: LinkRole,
    kind: ConnectKind,
    connection: Connection,
    // Whether the connection got as far as connecting, so that failing tells nothing about the address.
    connected: bool,
    // When the connection last received anything.
    last_received: Option<Instant>,
    // Set once the connection is replaced: It sends nothing more, and takes late answers until then.
    draining_until: Option<Instant>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum LookupWaiter {
    External(LookupId),
    // The server is given by name, and connecting to it waits for its address.
    ServerAddress(ConnectionId),
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum QueryWaiter {
    Lookup { key: String, index: usize },
    Validation(u64),
}

struct InFlightQuery {
    id: u16,
    question: Question,
    dnssec_ok: bool,
    // Set once a response came back truncated; the question is asked again over TCP from then on.
    over_tcp_fallback: bool,
    attempts: usize,
    waiters: Vec<QueryWaiter>,
    retry_at: Option<Instant>,
    // Sent while the connection was still coming up. The retry schedule starts once it is up.
    awaiting_connection: bool,
    // The connection the question last went out on.
    sent_on: Option<ConnectionId>,
}

struct PendingLookup {
    waiters: Vec<LookupWaiter>,
    name: String,
    domain_name: DomainName,
    class: Class,
    desired_types: Vec<ResourceType>,
    validate: bool,
    // Each with when it arrived, which is when the lifetimes of its records start.
    responses: Vec<Option<(Message, Instant)>>,
    validations: Vec<Option<ValidationResult>>,
}

// Validating one response of a lookup. Validation never waits: when it needs a DS or DNSKEY RRset it does not have yet,
// it says which, and runs again once that has arrived.
struct PendingValidation {
    lookup_key: String,
    index: usize,
    response: Message,
    question: Question,
    fetches: Fetches,
}

// Per-name state for a system-resolver lookup. It is split into an A and an AAAA side, so that a stub resolver that
// drops one half of a coupled query can't stall us on it. When one side has records first, the waiters get them after
// a grace window for the other side: briefly for AAAA (RFC 8305's Resolution Delay), and longer for A. The slower side
// keeps going and merges its records into the cache entry, so later lookups see the full set.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum FamilyLookup {
    NotAsked,
    Pending,
    Answered,
    Failed,
}

impl FamilyLookup {
    fn is_done(self) -> bool {
        self != FamilyLookup::Pending
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SystemResolutionPurpose {
    // A lookup, whose answers go in the cache.
    Lookup,
    // The addresses of the configured server, which stay out of the cache, as it only holds the server's answers.
    ServerAddress,
}

struct PendingSystemResolution {
    // Tells this resolution apart from a later one for the same name and types, which answers to it do not concern.
    id: u64,
    // Where the result goes in the cache, if it goes there at all.
    cache_key: Option<String>,
    // The configured server whose addresses these are, if they are, and its name still names it.
    server_host: Option<String>,
    entry: CacheEntry,
    waiters: Vec<LookupWaiter>,
    a: FamilyLookup,
    aaaa: FamilyLookup,
    settled: bool,
    grace_deadline: Option<Instant>,
    give_up_at: Instant,
}

pub struct Resolver {
    server: Option<ServerConfig>,
    // Where the server is, and which of those addresses to connect to next.
    server_addresses: Vec<SocketAddr>,
    next_server_address: usize,
    links: Vec<Link>,
    next_connection_id: ConnectionId,
    next_system_request_id: SystemRequestId,
    next_lookup_id: LookupId,

    cache: Cache,
    pending_lookups: HashMap<String, PendingLookup>,
    in_flight: HashMap<String, InFlightQuery>,
    in_flight_by_id: HashMap<u16, String>,
    system_resolutions: HashMap<String, PendingSystemResolution>,
    system_requests: HashMap<SystemRequestId, (String, u64, AddressFamily)>,
    system_resolver_timeout: Duration,
    // Counts resets, which a lookup of the server's name belongs to the latest of.
    connection_generation: u64,

    loopback_domains: Vec<String>,
    is_ipv6_globally_reachable: Box<dyn FnMut() -> bool>,
    ipv6_reachability_checked_at: Option<Instant>,
    ipv6_is_globally_reachable: bool,
    random: Box<dyn FnMut() -> Option<u16>>,

    validator: Validator,
    validations: HashMap<u64, PendingValidation>,
    next_validation_id: u64,
    // The wall clock at some instant, which DNSSEC signature validity is measured against.
    wall_clock: (Instant, u64),

    actions: VecDeque<Action>,
}

impl Resolver {
    /// `is_ipv6_globally_reachable` tells whether the host can reach the IPv6 internet; it is asked at most once a
    /// second. `random` picks query IDs, unless the random source fails.
    pub fn new(is_ipv6_globally_reachable: Box<dyn FnMut() -> bool>, random: Box<dyn FnMut() -> Option<u16>>) -> Self {
        Self {
            server: None,
            server_addresses: Vec::new(),
            next_server_address: 0,
            links: Vec::new(),
            next_connection_id: 1,
            next_system_request_id: 1,
            next_lookup_id: 1,
            cache: Cache::new(MAX_CACHE_ENTRIES),
            pending_lookups: HashMap::new(),
            in_flight: HashMap::new(),
            in_flight_by_id: HashMap::new(),
            system_resolutions: HashMap::new(),
            system_requests: HashMap::new(),
            system_resolver_timeout: SYSTEM_RESOLVER_TIMEOUT,
            connection_generation: 0,
            loopback_domains: Vec::new(),
            is_ipv6_globally_reachable,
            ipv6_reachability_checked_at: None,
            ipv6_is_globally_reachable: false,
            random,
            validator: Validator::new(),
            validations: HashMap::new(),
            next_validation_id: 1,
            wall_clock: (
                Instant::now(),
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .map_or(0, |since| since.as_secs()),
            ),
            actions: VecDeque::new(),
        }
    }

    /// What the wall clock said at `instant`, in seconds since the Unix epoch.
    pub fn set_wall_clock(&mut self, instant: Instant, unix_seconds: u64) {
        self.wall_clock = (instant, unix_seconds);
    }

    pub fn set_trust_anchors(&mut self, anchors: Vec<crate::message::Ds>) {
        self.validator.set_trust_anchors(anchors);
    }

    fn unix_now(&self, now: Instant) -> u32 {
        let (instant, unix) = self.wall_clock;
        let seconds = if now >= instant {
            unix.saturating_add(now.duration_since(instant).as_secs())
        } else {
            unix.saturating_sub(instant.duration_since(now).as_secs())
        };
        u32::try_from(seconds).unwrap_or(u32::MAX)
    }

    pub fn take_action(&mut self) -> Option<Action> {
        self.actions.pop_front()
    }

    /// When `on_tick()` next has something to do.
    pub fn next_deadline(&self) -> Option<Instant> {
        let links = self
            .links
            .iter()
            .flat_map(|link| [link.connection.deadline(), link.draining_until])
            .flatten();
        let retries = self.in_flight.values().filter_map(|query| query.retry_at);
        let system = self
            .system_resolutions
            .values()
            .flat_map(|state| [state.grace_deadline, Some(state.give_up_at)])
            .flatten();
        links.chain(retries).chain(system).min()
    }

    /// Without a server, every name goes to the system resolver.
    pub fn set_server(&mut self, mut server: Option<ServerConfig>, now: Instant) {
        // RFC 7858, 3.1. Session Initiation
        // By default, a DNS client desiring privacy from DNS over TLS from a particular server MUST establish a TCP
        // connection to port 853 on the server, unless it has mutual agreement with its server to use a port other
        // than port 853 for DNS over TLS.
        // Port 0 means the default one, of the transport in use.
        if let Some(server) = server.as_mut().filter(|server| server.port == 0) {
            server.port = match server.transport {
                ServerTransport::Tls => 853,
                ServerTransport::Udp => 53,
            };
        }
        if self.server == server {
            return;
        }
        self.server = server;
        self.server_addresses.clear();
        self.next_server_address = 0;
        self.reset_connection(now);
    }

    /// How long the system resolver may take to answer, for tests that cannot wait for it.
    pub fn set_system_resolver_timeout(&mut self, timeout: Duration) {
        self.system_resolver_timeout = timeout;
    }

    /// Names within these domains resolve to loopback, as names within "localhost" do.
    pub fn set_loopback_domains(&mut self, domains: Vec<String>) {
        self.loopback_domains = domains;
    }

    /// Drops the connection, everything in flight on it, and everything cached.
    pub fn reset_connection(&mut self, now: Instant) {
        for link in std::mem::take(&mut self.links) {
            self.actions.push_back(Action::Close { connection: link.id });
        }
        let keys: Vec<String> = self.in_flight.keys().cloned().collect();
        for key in keys {
            self.settle_query(&key, Err(Error::new("DNS connection was reset")), now);
        }
        self.cache.clear();
        self.validator.clear_caches();
        self.connection_generation += 1;
        // What the system resolver is still looking up answers the lookups waiting on it, but it is from before the
        // reset, so it stays out of the cache.
        for state in self.system_resolutions.values_mut() {
            state.cache_key = None;
            state.server_host = None;
        }
    }

    pub fn cached_entry_count(&self) -> usize {
        self.cache.len()
    }

    // ---- Lookups -----------------------------------------------------------------------------------------------

    pub fn lookup(
        &mut self,
        name: &str,
        class: Class,
        desired_types: &[ResourceType],
        options: LookupOptions,
        now: Instant,
    ) -> LookupId {
        let lookup = self.next_lookup_id;
        self.next_lookup_id += 1;
        self.start_lookup(LookupWaiter::External(lookup), name, class, desired_types, options, now);
        lookup
    }

    fn start_lookup(
        &mut self,
        waiter: LookupWaiter,
        name: &str,
        class: Class,
        desired_types: &[ResourceType],
        options: LookupOptions,
        now: Instant,
    ) {
        if let Ok(address) = name.parse::<Ipv4Addr>()
            && desired_types.contains(&ResourceType::A)
        {
            return self.settle_lookup_waiter(waiter, Ok(literal_result(RecordData::A(address), ResourceType::A)), now);
        }
        // A URL host writes an IPv6 address in brackets.
        let unbracketed = name
            .strip_prefix('[')
            .and_then(|name| name.strip_suffix(']'))
            .unwrap_or(name);
        if let Ok(address) = unbracketed.parse::<Ipv6Addr>()
            && desired_types.contains(&ResourceType::AAAA)
        {
            return self.settle_lookup_waiter(
                waiter,
                Ok(literal_result(RecordData::Aaaa(address), ResourceType::AAAA)),
                now,
            );
        }

        // RFC 1034, 3.1. Name space specifications and terminology.
        // Since a complete domain name ends with the root label, this leads to a printed form which ends in a dot.
        let name = name.strip_suffix('.').unwrap_or(name);

        // Anything still ending in a dot has an empty label, and must not slip past the suffix checks below.
        if name.ends_with('.') {
            return self.settle_lookup_waiter(waiter, Err(Error::new("Invalid domain name")), now);
        }

        let domain_name = DomainName::from_string(name);

        // https://www.rfc-editor.org/rfc/rfc6761#section-6.3
        // "localhost" and names within ".localhost" resolve to loopback for address queries and are never sent
        // upstream; we answer in-process since the host resolver and upstream server are not guaranteed to.
        let is_loopback_name = is_within_domain(name, "localhost")
            || self
                .loopback_domains
                .iter()
                .any(|domain| is_within_domain(name, domain));
        if is_loopback_name {
            let mut records = Vec::new();
            if desired_types.contains(&ResourceType::A) {
                records.push(synthetic_record(ResourceType::A, RecordData::A(Ipv4Addr::LOCALHOST), 0));
            }
            if desired_types.contains(&ResourceType::AAAA) {
                records.push(synthetic_record(
                    ResourceType::AAAA,
                    RecordData::Aaaa(Ipv6Addr::LOCALHOST),
                    0,
                ));
            }
            let result = Arc::new(LookupResult {
                name: domain_name,
                records,
                security_status: None,
            });
            return self.settle_lookup_waiter(waiter, Ok(result), now);
        }

        // Checked after the localhost names above, since those never go on the wire and URL hosts may have longer labels.
        if !domain_name.is_valid() {
            return self.settle_lookup_waiter(waiter, Err(Error::new("Invalid domain name")), now);
        }

        // RFC 7686, 2. The ".onion" Special-Use Domain Name.
        // Applications that do not implement the Tor protocol SHOULD generate an error upon the use of .onion and
        // SHOULD NOT perform a DNS lookup.
        if is_within_domain(name, "onion") {
            return self.settle_lookup_waiter(waiter, Err(Error::new("Refusing to resolve a .onion name")), now);
        }

        // RFC 6762, 3. Multicast DNS Names.
        // Any DNS query for a name ending with ".local." MUST be sent to the mDNS IPv4 link-local multicast address
        // 224.0.0.251 (or its IPv6 equivalent FF02::FB).
        // The system resolver takes care of that, so these never go to the configured server.
        let is_link_local_name = is_within_domain(name, "local");
        // The system resolver looks up the configured server itself. What it finds is not an answer from that server,
        // so it stays out of the cache, which only holds those.
        let is_server_address = matches!(waiter, LookupWaiter::ServerAddress(_));
        let uses_system_resolver = is_link_local_name || self.server.is_none() || is_server_address;

        // NB: IPv6 addresses are no use to a host that cannot reach the IPv6 internet, so the system resolver skips
        //     the AAAA side there. Link-local IPv6 addresses work without that, so ".local" names keep the AAAA side.
        //     A cached result without AAAA only serves while IPv6 stays unreachable.
        let skips_aaaa = uses_system_resolver
            && !is_link_local_name
            && !options.query_aaaa_without_global_ipv6
            && desired_types.contains(&ResourceType::A)
            && desired_types.contains(&ResourceType::AAAA)
            && !self.ipv6_globally_reachable(now);
        let queried_types: Vec<ResourceType> = desired_types
            .iter()
            .copied()
            .filter(|rtype| !(skips_aaaa && *rtype == ResourceType::AAAA))
            .collect();

        if !is_server_address
            && let Some(entry) = self.cache.lookup(name, &queried_types, now)
            && (!options.validate_dnssec_locally || Cache::was_validated(entry))
        {
            let result = entry.snapshot_for(&queried_types);
            return self.settle_lookup_waiter(waiter, Ok(result), now);
        }

        if uses_system_resolver {
            if !is_link_local_name && options.validate_dnssec_locally {
                return self.settle_lookup_waiter(
                    waiter,
                    Err(Error::new("No connection available to validate DNSSEC")),
                    now,
                );
            }
            let purpose = if is_server_address {
                SystemResolutionPurpose::ServerAddress
            } else {
                SystemResolutionPurpose::Lookup
            };
            return self.resolve_with_system_resolver(waiter, name, queried_types, purpose, now);
        }

        // Nothing to ask for is answered at once, with nothing.
        if desired_types.is_empty() {
            let result = Arc::new(LookupResult {
                name: domain_name,
                records: Vec::new(),
                security_status: None,
            });
            return self.settle_lookup_waiter(waiter, Ok(result), now);
        }

        // Every caller for the same name, types and validation policy shares one lookup; the first one runs it.
        let mut key = format!("{name}|{}|{}", class.0, options.validate_dnssec_locally);
        for rtype in desired_types {
            key.push_str(&format!("|{}", rtype.0));
        }

        if let Some(pending) = self.pending_lookups.get_mut(&key) {
            pending.waiters.push(waiter);
            return;
        }

        self.pending_lookups.insert(
            key.clone(),
            PendingLookup {
                waiters: vec![waiter],
                name: name.to_owned(),
                domain_name: domain_name.clone(),
                class,
                desired_types: desired_types.to_vec(),
                validate: options.validate_dnssec_locally,
                responses: vec![None; desired_types.len()],
                validations: vec![None; desired_types.len()],
            },
        );
        for (index, rtype) in desired_types.iter().enumerate() {
            let waiter = QueryWaiter::Lookup {
                key: key.clone(),
                index,
            };
            self.query(
                &domain_name,
                *rtype,
                class,
                options.validate_dnssec_locally,
                waiter,
                now,
            );
            // A query that failed right away has settled the whole lookup already.
            if !self.pending_lookups.contains_key(&key) {
                return;
            }
        }
    }

    fn settle_lookup_waiter(&mut self, waiter: LookupWaiter, result: Result<Arc<LookupResult>, Error>, now: Instant) {
        match waiter {
            LookupWaiter::External(lookup) => self.actions.push_back(Action::LookupDone { lookup, result }),
            LookupWaiter::ServerAddress(connection) => self.server_address_resolved(connection, result, now),
        }
    }

    fn on_lookup_response(&mut self, key: &str, index: usize, result: Result<Message, Error>, now: Instant) {
        let Some(pending) = self.pending_lookups.get_mut(key) else {
            return;
        };
        let message = match result {
            Ok(message) => message,
            Err(error) => return self.settle_pending_lookup(key, Err(error), now),
        };
        pending.responses[index] = Some((message, now));
        if pending.responses.iter().any(Option::is_none) {
            return;
        }

        if pending.validate {
            let pending = self.pending_lookups.get_mut(key).expect("pending lookup");
            let mut started = Vec::new();
            for (index, rtype) in pending.desired_types.iter().enumerate() {
                let id = self.next_validation_id;
                self.next_validation_id += 1;
                let question = Question {
                    name: pending.domain_name.clone(),
                    rtype: *rtype,
                    class: pending.class,
                };
                let (response, _) = pending.responses[index].clone().expect("response");
                self.validations.insert(
                    id,
                    PendingValidation {
                        lookup_key: key.to_owned(),
                        index,
                        response,
                        question,
                        fetches: Fetches::new(),
                    },
                );
                started.push(id);
            }
            for id in started {
                self.run_validation(id, now);
            }
            return;
        }

        let result = self.finish_lookup(key, now);
        self.settle_pending_lookup(key, result, now);
    }

    fn run_validation(&mut self, id: u64, now: Instant) {
        let unix_now = self.unix_now(now);
        let Some(validation) = self.validations.get(&id) else {
            return;
        };
        let step = self.validator.validate(
            &validation.response,
            &validation.question,
            &validation.fetches,
            unix_now,
        );
        match step {
            Step::NeedFetch(question) => self.query(
                &question.name,
                question.rtype,
                Class::IN,
                true,
                QueryWaiter::Validation(id),
                now,
            ),
            Step::Failed(_) => {
                let validation = self.validations.remove(&id).expect("validation");
                self.fail_validated_lookup(&validation.lookup_key, Error::new("DNSSEC validation failed"), now);
            }
            Step::Done(result) => {
                let validation = self.validations.remove(&id).expect("validation");
                let key = validation.lookup_key;
                let Some(pending) = self.pending_lookups.get_mut(&key) else {
                    return;
                };
                pending.validations[validation.index] = Some(result);
                if pending.validations.iter().any(Option::is_none) {
                    return;
                }
                let result = self.finish_validated_lookup(&key, now);
                self.settle_pending_lookup(&key, result, now);
            }
        }
    }

    fn on_validation_fetch(&mut self, id: u64, result: Result<Message, Error>, now: Instant) {
        let received_at = self.unix_now(now);
        let Some(validation) = self.validations.get_mut(&id) else {
            return;
        };
        match result {
            Ok(message) => {
                let question = message.questions.first().cloned();
                if let Some(question) = question {
                    validation.fetches.insert_received_at(&question, message, received_at);
                }
                self.run_validation(id, now);
            }
            Err(error) => {
                // A fetch that failed, as one does once the server stops answering, says nothing of the records it was
                // for: The lookup fails with the fetch's own error, rather than as if validation had found them bogus.
                let validation = self.validations.remove(&id).expect("validation");
                self.fail_validated_lookup(&validation.lookup_key, error, now);
            }
        }
    }

    fn fail_validated_lookup(&mut self, key: &str, error: Error, now: Instant) {
        self.validations.retain(|_, validation| validation.lookup_key != key);
        self.settle_pending_lookup(key, Err(error), now);
    }

    fn finish_validated_lookup(&mut self, key: &str, now: Instant) -> Result<Arc<LookupResult>, Error> {
        let pending = self.pending_lookups.get_mut(key).expect("pending lookup");
        let mut entry = CacheEntry::new(pending.domain_name.clone());
        let mut status = SecurityStatus::Secure;
        for (index, rtype) in pending.desired_types.iter().enumerate() {
            let validation = pending.validations[index].take().expect("validation");
            // NB: Validation leaves no record more time than its TTL from when the response arrived. Signatures that
            //     expire sooner shorten that further, from the time of validation, which this counts from too.
            let received_at = pending.responses[index].as_ref().expect("response").1;
            match validation.status {
                dnssec::SecurityStatus::Bogus => return Err(Error::new("DNSSEC validation failed")),
                dnssec::SecurityStatus::Insecure => status = SecurityStatus::Insecure,
                dnssec::SecurityStatus::Secure => {}
            }
            // A negative answer holds no longer than the aliases leading to it, nor than its authenticated proof.
            let ttl_limit = [alias_ttl(&validation.records), validation.denial_ttl]
                .into_iter()
                .flatten()
                .min();
            for record in validation.records {
                entry.add_record(record, received_at);
            }
            if let Some((response, _)) = &pending.responses[index] {
                add_negative_answer(&mut entry, *rtype, response, ttl_limit, received_at);
            }
        }
        entry.finished_request();
        entry.set_security_status(status);
        // NB: The lookup gets every record it fetched, even one that has expired since, as the cache does not.
        let result = entry.snapshot();
        let name = pending.name.clone();
        self.cache.insert(name, entry, now);
        Ok(result)
    }

    fn finish_lookup(&mut self, key: &str, now: Instant) -> Result<Arc<LookupResult>, Error> {
        let pending = self.pending_lookups.get_mut(key).expect("pending lookup");
        let mut entry = CacheEntry::new(pending.domain_name.clone());
        for (index, rtype) in pending.desired_types.iter().enumerate() {
            let (response, received_at) = pending.responses[index].take().expect("response");
            let question = Question {
                name: pending.domain_name.clone(),
                rtype: *rtype,
                class: pending.class,
            };
            let rcode = response.header.options.response_code();
            if rcode != ResponseCode::NO_ERROR && rcode != ResponseCode::NAME_ERROR {
                return Err(Error::new("DNS server returned an error"));
            }
            let answers = response.answers_to(&question);
            let ttl_limit = alias_ttl(&answers);
            for record in answers {
                entry.add_record(record, received_at);
            }
            add_negative_answer(&mut entry, *rtype, &response, ttl_limit, received_at);
        }
        entry.finished_request();
        // NB: The lookup gets every record it fetched, even one that has expired since, as the cache does not.
        let result = entry.snapshot();
        let name = pending.name.clone();
        self.cache.insert(name, entry, now);
        Ok(result)
    }

    fn settle_pending_lookup(&mut self, key: &str, result: Result<Arc<LookupResult>, Error>, now: Instant) {
        let Some(pending) = self.pending_lookups.remove(key) else {
            return;
        };
        for waiter in pending.waiters {
            self.settle_lookup_waiter(waiter, result.clone(), now);
        }
    }

    fn ipv6_globally_reachable(&mut self, now: Instant) -> bool {
        let stale = self
            .ipv6_reachability_checked_at
            .is_none_or(|checked_at| now.duration_since(checked_at) >= IPV6_REACHABILITY_CHECK_INTERVAL);
        if stale {
            self.ipv6_is_globally_reachable = (self.is_ipv6_globally_reachable)();
            self.ipv6_reachability_checked_at = Some(now);
        }
        self.ipv6_is_globally_reachable
    }

    // ---- The system resolver -----------------------------------------------------------------------------------

    fn resolve_with_system_resolver(
        &mut self,
        waiter: LookupWaiter,
        name: &str,
        queried_types: Vec<ResourceType>,
        purpose: SystemResolutionPurpose,
        now: Instant,
    ) {
        // The system resolver only knows addresses.
        let wants = |rtype: ResourceType| queried_types.contains(&rtype) || queried_types.contains(&ResourceType::ANY);
        let (wants_a, wants_aaaa) = (wants(ResourceType::A), wants(ResourceType::AAAA));
        if !wants_a && !wants_aaaa {
            let error = Error::new("The system resolver can only look up addresses");
            return self.settle_lookup_waiter(waiter, Err(error), now);
        }

        // NB: A resolution only serves callers that query the same types, so that a caller wanting AAAA never joins
        //     one that skips it, and one that skips AAAA never joins one that went ahead without A.
        let mut sorted_types = queried_types.clone();
        sorted_types.sort_by_key(|rtype| rtype.0);
        let mut key = name.to_owned();
        for rtype in &sorted_types {
            key.push_str(&format!("|{}", rtype.0));
        }
        // A resolution that stays out of the cache never serves one that goes in it, nor the other way around. Nor does
        // one of the server's name from before a reset, such as for another port, serve one after it.
        if purpose == SystemResolutionPurpose::ServerAddress {
            key.push_str(&format!("|uncached|{}", self.connection_generation));
        }

        if let Some(state) = self.system_resolutions.get_mut(&key) {
            if !state.settled {
                state.waiters.push(waiter);
                return;
            }
            // The grace window settled the resolution while the slower side is still going. Serve the joining
            // caller from what is there, as long as some of it is still fresh. The slower side may never answer, so
            // once nothing is, start over.
            state.entry.expire(now);
            if state.entry.has_records() {
                let result = Ok(state.entry.snapshot());
                return self.settle_lookup_waiter(waiter, result, now);
            }
            if let Some(state) = self.system_resolutions.remove(&key) {
                self.forget_system_requests(&key, state.id);
            }
        }

        let entry = CacheEntry::new(DomainName::from_string(name));
        let id = self.next_system_request_id;
        self.next_system_request_id += 1;
        self.system_resolutions.insert(
            key.clone(),
            PendingSystemResolution {
                id,
                cache_key: (purpose == SystemResolutionPurpose::Lookup).then(|| name.to_owned()),
                server_host: (purpose == SystemResolutionPurpose::ServerAddress).then(|| name.to_owned()),
                entry,
                waiters: vec![waiter],
                a: if wants_a {
                    FamilyLookup::Pending
                } else {
                    FamilyLookup::NotAsked
                },
                aaaa: if wants_aaaa {
                    FamilyLookup::Pending
                } else {
                    FamilyLookup::NotAsked
                },
                settled: false,
                grace_deadline: None,
                give_up_at: now + self.system_resolver_timeout,
            },
        );

        if wants_a {
            self.ask_system_resolver(&key, id, name, AddressFamily::V4);
        }
        if wants_aaaa {
            self.ask_system_resolver(&key, id, name, AddressFamily::V6);
        }
    }

    fn ask_system_resolver(&mut self, key: &str, id: u64, name: &str, family: AddressFamily) {
        let request = self.next_system_request_id;
        self.next_system_request_id += 1;
        self.system_requests.insert(request, (key.to_owned(), id, family));
        self.actions.push_back(Action::SystemResolve {
            request,
            name: name.to_owned(),
            family,
        });
    }

    /// The system resolver answered a `SystemResolve` action.
    pub fn on_system_resolved(&mut self, request: SystemRequestId, result: Result<Vec<IpAddr>, String>, now: Instant) {
        let Some((key, id, family)) = self.system_requests.remove(&request) else {
            return;
        };
        let Some(state) = self.system_resolutions.get_mut(&key).filter(|state| state.id == id) else {
            return;
        };

        let outcome = if result.is_ok() {
            FamilyLookup::Answered
        } else {
            FamilyLookup::Failed
        };
        match family {
            AddressFamily::V4 => state.a = outcome,
            AddressFamily::V6 => state.aaaa = outcome,
        }

        // Late-arriving records still populate the cache for later lookups.
        let mut got_records = false;
        for address in result.unwrap_or_default() {
            let record = match address {
                IpAddr::V4(address) => synthetic_record(
                    ResourceType::A,
                    RecordData::A(address),
                    SYSTEM_RESOLVER_SYNTHETIC_TTL_SECONDS,
                ),
                IpAddr::V6(address) => synthetic_record(
                    ResourceType::AAAA,
                    RecordData::Aaaa(address),
                    SYSTEM_RESOLVER_SYNTHETIC_TTL_SECONDS,
                ),
            };
            state.entry.add_record(record, now);
            got_records = true;
        }

        if state.a.is_done() && state.aaaa.is_done() {
            return self.finish_system_resolution(&key, now);
        }

        if state.settled || !got_records || state.grace_deadline.is_some() {
            return;
        }

        // Give the other address family time to add its records.
        let delay = match family {
            AddressFamily::V4 => IPV6_RESOLUTION_DELAY,
            AddressFamily::V6 => IPV4_RESOLUTION_DELAY,
        };
        state.grace_deadline = Some(now + delay);
    }

    fn give_up_on_system_resolution(&mut self, key: &str, now: Instant) {
        let Some(state) = self.system_resolutions.get_mut(key) else {
            return;
        };
        for lookup in [&mut state.a, &mut state.aaaa] {
            if *lookup == FamilyLookup::Pending {
                *lookup = FamilyLookup::Failed;
            }
        }
        self.finish_system_resolution(key, now);
    }

    // Answers that come in for a resolution that is gone find no request to go with, as if they never came.
    fn forget_system_requests(&mut self, key: &str, id: u64) {
        let forgotten: Vec<SystemRequestId> = self
            .system_requests
            .iter()
            .filter(|(_, (request_key, request_id, _))| request_key == key && *request_id == id)
            .map(|(request, _)| *request)
            .collect();
        for request in forgotten {
            self.system_requests.remove(&request);
            self.actions.push_back(Action::CancelSystemResolve { request });
        }
    }

    fn finish_system_resolution(&mut self, key: &str, now: Instant) {
        let Some(mut state) = self.system_resolutions.remove(key) else {
            return;
        };
        self.forget_system_requests(key, state.id);

        // A family the system resolver looked up and found no addresses of has none, as far as it knows. It says
        // nothing about for how long, so that holds as long as addresses it finds would. A family it failed to look up,
        // or was not asked about, may well have some.
        let negative_expiration = now + Duration::from_secs(u64::from(SYSTEM_RESOLVER_SYNTHETIC_TTL_SECONDS));
        for (lookup, rtype) in [(state.a, ResourceType::A), (state.aaaa, ResourceType::AAAA)] {
            if lookup == FamilyLookup::Answered && !state.entry.has_record_of_type(rtype) {
                state.entry.add_negative_answer(rtype, negative_expiration);
            }
        }
        state.entry.finished_request();

        let waiters = std::mem::take(&mut state.waiters);
        // Every family it was asked about having no addresses is an answer, as when a DNS server says so. A family
        // it failed to look up leaves the lookup failed, unless the other found some.
        let answered = [state.a, state.aaaa]
            .iter()
            .all(|lookup| matches!(lookup, FamilyLookup::Answered | FamilyLookup::NotAsked));
        let result = (!state.settled).then(|| (state.entry.has_records() || answered).then(|| state.entry.snapshot()));

        // Connecting to the server went ahead with the addresses it had then. The ones that came in since are worth
        // trying too, should those not work.
        if state.settled
            && let Some(host) = &state.server_host
            && let Some(server) = self.server.as_ref().filter(|server| &server.host == host)
        {
            let port = server.port;
            for address in state.entry.snapshot().addresses() {
                let address = SocketAddr::new(address, port);
                if !self.server_addresses.contains(&address) {
                    self.server_addresses.push(address);
                }
            }
        }

        // What the resolution found first may be older than what another one has cached since, so it only fills in
        // what the cache does not know.
        if let Some(cache_key) = state.cache_key {
            self.cache.fill_in(cache_key, state.entry, now);
        }
        if let Some(result) = result {
            self.settle_system_waiters(waiters, result, now);
        }
    }

    fn settle_system_resolution_early(&mut self, key: &str, now: Instant) {
        let Some(state) = self.system_resolutions.get_mut(key) else {
            return;
        };
        if state.settled {
            return;
        }
        state.settled = true;
        state.grace_deadline = None;
        let waiters = std::mem::take(&mut state.waiters);
        let result = state.entry.has_records().then(|| state.entry.snapshot());
        self.settle_system_waiters(waiters, result, now);
    }

    fn settle_system_waiters(&mut self, waiters: Vec<LookupWaiter>, result: Option<Arc<LookupResult>>, now: Instant) {
        let result = result.ok_or(Error::new("Could not resolve to IPv4 or IPv6 address"));
        for waiter in waiters {
            self.settle_lookup_waiter(waiter, result.clone(), now);
        }
    }

    // ---- Queries -----------------------------------------------------------------------------------------------

    // Sends one question. Identical questions in flight share one transaction.
    fn query(
        &mut self,
        name: &DomainName,
        rtype: ResourceType,
        class: Class,
        dnssec_ok: bool,
        waiter: QueryWaiter,
        now: Instant,
    ) {
        // NB: The labels are kept apart, as a label can have a dot in it, so that "a.b" in "example" is not "b" in
        //     "example" under "a".
        let key = format!("{:?}|{}|{}|{dnssec_ok}", name.canonicalized().labels, rtype.0, class.0);
        if let Some(existing) = self.in_flight.get_mut(&key) {
            existing.waiters.push(waiter);
            return;
        }

        let id = match self.free_query_id() {
            Ok(id) => id,
            Err(error) => return self.settle_query_waiter(waiter, Err(error), now),
        };

        self.in_flight.insert(
            key.clone(),
            InFlightQuery {
                id,
                question: Question {
                    name: name.clone(),
                    rtype,
                    class,
                },
                dnssec_ok,
                over_tcp_fallback: false,
                attempts: 0,
                waiters: vec![waiter],
                retry_at: None,
                awaiting_connection: false,
                sent_on: None,
            },
        );
        self.in_flight_by_id.insert(id, key.clone());

        if let Err(error) = self.send_query(&key, now) {
            self.settle_query(&key, Err(error), now);
        }
    }

    fn send_query(&mut self, key: &str, now: Instant) -> Result<(), Error> {
        let query = self.in_flight.get_mut(key).expect("in-flight query");
        if query.attempts >= QUERY_MAX_ATTEMPTS {
            return Err(Error::new("DNS lookup timed out"));
        }
        query.attempts += 1;

        let mut message = Message::default();
        message.header.id = query.id;
        message.header.question_count = 1;
        let mut options = Options::default();
        options.set_response_code(ResponseCode::NO_ERROR);
        options.set_recursion_desired(true);
        options.set_op_code(OpCode::QUERY);
        message.questions.push(query.question.clone());

        if query.dnssec_ok {
            options.set_checking_disabled(true);
            options.set_authenticated_data(true);
            message.header.additional_count = 1;
            let mut opt = Opt {
                udp_payload_size: 4096,
                ..Opt::default()
            };
            opt.set_dnssec_ok(true);
            message.additional_records.push(ResourceRecord {
                name: DomainName::root(),
                rtype: ResourceType::OPT,
                class: query.question.class,
                ttl: 0,
                data: RecordData::Opt(opt),
                raw: None,
            });
        }
        message.header.options = options;

        let bytes = message
            .to_wire()
            .map_err(|_| Error::new("Could not encode the DNS query"))?;
        let role = if query.over_tcp_fallback {
            LinkRole::TcpFallback
        } else {
            LinkRole::Primary
        };

        let index = self.ensure_link(role, now)?;
        let link = &mut self.links[index];
        link.connection.send(bytes)?;
        let ready = link.connection.is_ready();
        let link_id = link.id;

        let query = self.in_flight.get_mut(key).expect("in-flight query");
        query.sent_on = Some(link_id);
        query.awaiting_connection = !ready;
        query.retry_at = ready.then_some(now + QUERY_RETRY_INTERVAL);
        self.flush_link(index);
        Ok(())
    }

    fn retry_query(&mut self, key: &str, now: Instant) {
        if let Err(error) = self.send_query(key, now) {
            self.settle_query(key, Err(error), now);
        }
    }

    fn settle_query(&mut self, key: &str, result: Result<Message, Error>, now: Instant) {
        let Some(query) = self.in_flight.remove(key) else {
            return;
        };
        self.in_flight_by_id.remove(&query.id);
        // Remove the query before notifying waiters, which may issue it again.
        for waiter in query.waiters {
            self.settle_query_waiter(waiter, result.clone(), now);
        }
    }

    fn settle_query_waiter(&mut self, waiter: QueryWaiter, result: Result<Message, Error>, now: Instant) {
        match waiter {
            QueryWaiter::Lookup { key, index } => self.on_lookup_response(&key, index, result, now),
            QueryWaiter::Validation(id) => self.on_validation_fetch(id, result, now),
        }
    }

    // A random ID that no query in flight has. Once random tries keep hitting ones in use, look for a free one, which
    // there may not be.
    fn free_query_id(&mut self) -> Result<u16, Error> {
        const RANDOM_TRIES: usize = 16;
        let mut id = 0;
        for _ in 0..RANDOM_TRIES {
            id = (self.random)().ok_or(Error::new("Could not pick a random DNS query ID"))?;
            if !self.in_flight_by_id.contains_key(&id) {
                return Ok(id);
            }
        }
        (1..=u16::MAX)
            .map(|offset| id.wrapping_add(offset))
            .find(|id| !self.in_flight_by_id.contains_key(id))
            .ok_or(Error::new("Too many DNS queries are in flight"))
    }

    // ---- Connections -------------------------------------------------------------------------------------------

    fn find_link(&self, id: ConnectionId) -> Option<usize> {
        self.links.iter().position(|link| link.id == id)
    }

    fn ensure_link(&mut self, role: LinkRole, now: Instant) -> Result<usize, Error> {
        if let Some(index) = self
            .links
            .iter()
            .position(|link| link.role == role && link.draining_until.is_none())
        {
            return Ok(index);
        }

        let Some(server) = self.server.clone() else {
            return Err(Error::new("No connection to the configured DNS server"));
        };
        let kind = match (role, server.transport) {
            (LinkRole::Primary, ServerTransport::Udp) => ConnectKind::Udp,
            (LinkRole::TcpFallback, ServerTransport::Udp) => ConnectKind::Tcp,
            (LinkRole::Primary, ServerTransport::Tls) => ConnectKind::Tls,
            (LinkRole::TcpFallback, ServerTransport::Tls) => {
                return Err(Error::new(
                    "No TCP connection to the configured DNS server is available",
                ));
            }
        };

        let id = self.next_connection_id;
        self.next_connection_id += 1;
        let transport_kind = if kind == ConnectKind::Udp {
            transport::Kind::Udp
        } else {
            transport::Kind::Stream
        };
        self.links.push(Link {
            id,
            role,
            kind,
            connection: Connection::new(transport_kind, now, CONNECT_TIMEOUT),
            connected: false,
            last_received: None,
            draining_until: None,
        });

        if self.server_addresses.is_empty()
            && let Ok(address) = server.host.parse::<IpAddr>()
        {
            self.server_addresses.push(SocketAddr::new(address, server.port));
        }

        // The fallback goes where the primary connection went.
        let address = self.server_addresses.get(self.next_server_address).copied();
        match address {
            Some(address) => self.actions.push_back(Action::Connect {
                connection: id,
                kind,
                address,
                server_name: server.host,
            }),
            None => {
                let types = [ResourceType::A, ResourceType::AAAA];
                let options = LookupOptions::default();
                self.start_lookup(
                    LookupWaiter::ServerAddress(id),
                    &server.host,
                    Class::IN,
                    &types,
                    options,
                    now,
                );
            }
        }

        self.find_link(id)
            .ok_or(Error::new("Failed to resolve DNS server hostname"))
    }

    fn server_address_resolved(
        &mut self,
        connection: ConnectionId,
        result: Result<Arc<LookupResult>, Error>,
        now: Instant,
    ) {
        let Some(index) = self.find_link(connection) else {
            return;
        };
        let Some(server) = self.server.clone() else { return };
        let addresses = result.map(|result| result.addresses()).unwrap_or_default();
        let Some(address) = addresses.first() else {
            self.links[index]
                .connection
                .on_socket_error("Failed to resolve DNS server hostname".to_owned());
            return self.process_link(index, now);
        };
        let address = SocketAddr::new(*address, server.port);
        self.server_addresses = addresses
            .iter()
            .map(|address| SocketAddr::new(*address, server.port))
            .collect();
        self.next_server_address = 0;
        let kind = self.links[index].kind;
        self.actions.push_back(Action::Connect {
            connection,
            kind,
            address,
            server_name: server.host,
        });
    }

    pub fn on_connected(&mut self, connection: ConnectionId, now: Instant) {
        let Some(index) = self.find_link(connection) else {
            return;
        };
        self.links[index].connection.on_connected();
        self.links[index].connected = true;
        let role = self.links[index].role;
        for query in self.in_flight.values_mut() {
            let query_role = if query.over_tcp_fallback {
                LinkRole::TcpFallback
            } else {
                LinkRole::Primary
            };
            if query.awaiting_connection && query_role == role {
                query.awaiting_connection = false;
                query.retry_at = Some(now + QUERY_RETRY_INTERVAL);
            }
        }
        self.process_link(index, now);
    }

    pub fn on_connect_failed(&mut self, connection: ConnectionId, error: String, now: Instant) {
        let Some(index) = self.find_link(connection) else {
            return;
        };
        self.links[index].connection.on_socket_error(error);
        self.process_link(index, now);
    }

    /// For UDP, `bytes` is one datagram. For streams, it is whatever the socket read.
    pub fn on_received(&mut self, connection: ConnectionId, bytes: &[u8], now: Instant) {
        let Some(index) = self.find_link(connection) else {
            return;
        };
        self.links[index].connection.on_received(bytes);
        self.links[index].last_received = Some(now);
        self.process_link(index, now);
    }

    pub fn on_eof(&mut self, connection: ConnectionId, now: Instant) {
        let Some(index) = self.find_link(connection) else {
            return;
        };
        self.links[index].connection.on_eof();
        self.process_link(index, now);
    }

    pub fn on_tick(&mut self, now: Instant) {
        let ids: Vec<ConnectionId> = self.links.iter().map(|link| link.id).collect();
        for id in ids {
            if let Some(index) = self.find_link(id) {
                self.links[index].connection.on_tick(now);
                self.process_link(index, now);
            }
        }

        let due: Vec<String> = self
            .in_flight
            .iter()
            .filter(|(_, query)| query.retry_at.is_some_and(|at| at <= now))
            .map(|(key, _)| key.clone())
            .collect();
        self.replace_silent_udp_link(&due, now);
        for key in due {
            self.retry_query(&key, now);
        }
        self.close_drained_links(now);

        let due: Vec<String> = self
            .system_resolutions
            .iter()
            .filter(|(_, state)| state.grace_deadline.is_some_and(|at| at <= now))
            .map(|(key, _)| key.clone())
            .collect();
        for key in due {
            self.settle_system_resolution_early(&key, now);
        }

        let due: Vec<String> = self
            .system_resolutions
            .iter()
            .filter(|(_, state)| state.give_up_at <= now)
            .map(|(key, _)| key.clone())
            .collect();
        for key in due {
            self.give_up_on_system_resolution(&key, now);
        }
    }

    // A connected UDP socket keeps the source address it got when it connected. Once the network changes (from Wi-Fi to
    // a phone's tether, e.g.) while that address stays on its interface, sending still works, but no answer comes back
    // to that address. So a query due for a retry on a connection that has received nothing since the query went out
    // goes out again on a new connection, which takes the address that the network has now. The old connection still
    // takes answers for a while, since a slow server may yet answer what went out on it.
    fn replace_silent_udp_link(&mut self, due: &[String], now: Instant) {
        let Some(index) = self.links.iter().position(|link| {
            link.role == LinkRole::Primary
                && link.kind == ConnectKind::Udp
                && link.connected
                && link.draining_until.is_none()
        }) else {
            return;
        };
        let Link { id, last_received, .. } = self.links[index];
        // NB: Only a query that went out on this connection says anything about it.
        let has_gone_unanswered = due.iter().any(|key| {
            self.in_flight.get(key).is_some_and(|query| {
                let Some(sent_at) = query.retry_at.map(|at| at - QUERY_RETRY_INTERVAL) else {
                    return false;
                };
                query.sent_on == Some(id) && last_received.is_none_or(|received| received < sent_at)
            })
        });
        if has_gone_unanswered {
            self.links[index].draining_until = Some(now + REPLACED_CONNECTION_DRAIN_TIME);
        }
    }

    // Closes the replaced connections whose time is up, and all of them once no question is waiting for an answer.
    fn close_drained_links(&mut self, now: Instant) {
        let nothing_in_flight = self.in_flight.is_empty();
        let actions = &mut self.actions;
        self.links.retain(|link| {
            let drained = link
                .draining_until
                .is_some_and(|until| nothing_in_flight || until <= now);
            if drained {
                actions.push_back(Action::Close { connection: link.id });
            }
            !drained
        });
    }

    fn flush_link(&mut self, index: usize) {
        let link = &mut self.links[index];
        while let Some(bytes) = link.connection.take_outgoing() {
            self.actions.push_back(Action::Write {
                connection: link.id,
                bytes,
            });
        }
    }

    fn process_link(&mut self, index: usize, now: Instant) {
        self.flush_link(index);
        let id = self.links[index].id;

        while let Some(index) = self.find_link(id) {
            let Some(bytes) = self.links[index].connection.take_message() else {
                break;
            };
            // A message that does not parse answers nothing. Over a stream, its length still says where the next one
            // starts, so the ones after it are as good as the ones before.
            if let Ok(message) = Message::from_wire(&bytes) {
                self.on_message(id, message, now);
            }
        }

        let Some(index) = self.find_link(id) else { return };
        let Some(error) = self.links[index].connection.error().cloned() else {
            return;
        };

        let link = self.links.remove(index);
        self.actions.push_back(Action::Close { connection: link.id });
        // NB: A replaced connection has no questions of its own waiting to go out on it.
        if link.draining_until.is_some() {
            return;
        }

        // The server may be reachable at another of its addresses, such as over IPv4 when IPv6 does not work here.
        // Once all of them have failed, look the server's name up again, as its addresses may have changed.
        if link.role == LinkRole::Primary && !link.connected && !self.server_addresses.is_empty() {
            self.next_server_address += 1;
            if self.next_server_address == self.server_addresses.len() {
                self.next_server_address = 0;
                let named = self
                    .server
                    .as_ref()
                    .is_some_and(|server| server.host.parse::<IpAddr>().is_err());
                if named {
                    self.server_addresses.clear();
                }
            }
        }

        // Questions that never went out fail with the connection. Ones that did keep their retry schedule, so they get
        // sent again on the next connection.
        let role = link.role;
        let stranded: Vec<String> = self
            .in_flight
            .iter()
            .filter(|(_, query)| {
                query.awaiting_connection
                    && (if query.over_tcp_fallback {
                        LinkRole::TcpFallback
                    } else {
                        LinkRole::Primary
                    }) == role
            })
            .map(|(key, _)| key.clone())
            .collect();
        let error = Error::from(error);
        for key in stranded {
            self.settle_query(&key, Err(error.clone()), now);
        }
    }

    fn on_message(&mut self, connection: ConnectionId, message: Message, now: Instant) {
        if message.header.options.is_question() {
            return;
        }
        let Some(key) = self.in_flight_by_id.get(&message.header.id).cloned() else {
            return;
        };
        let Some(link_index) = self.find_link(connection) else {
            return;
        };
        let (arrived_on, arrived_over_udp) = (
            self.links[link_index].role,
            self.links[link_index].kind == ConnectKind::Udp,
        );
        let Some(query) = self.in_flight.get_mut(&key) else {
            return;
        };

        // Once a question is asked again over TCP, only the answer there counts, and not a late one over UDP.
        let asked_on = if query.over_tcp_fallback {
            LinkRole::TcpFallback
        } else {
            LinkRole::Primary
        };
        if arrived_on != asked_on {
            return;
        }

        // The response has to echo our question; anything else is not an answer to it.
        let Some(question) = message.questions.first() else {
            return;
        };
        if question.rtype != query.question.rtype
            || question.class != query.question.class
            || !question.name.eq_ignore_case(&query.question.name)
        {
            return;
        }

        // RFC 2181, 9. The TC (truncated) header bit.
        // When a DNS client receives a reply with TC set, it should ignore that response, and query again, using a
        // mechanism, such as a TCP connection, that will permit larger replies.
        if message.header.options.is_truncated() {
            if !arrived_over_udp {
                return self.settle_query(&key, Err(Error::new("DNS response was truncated")), now);
            }
            query.over_tcp_fallback = true;
            query.attempts = 0;
            return self.retry_query(&key, now);
        }

        self.settle_query(&key, Ok(message), now);
    }
}

fn is_within_domain(name: &str, domain: &str) -> bool {
    // RFC 4343, Abstract.
    // Domain Name System (DNS) names are "case insensitive".
    if name.eq_ignore_ascii_case(domain) {
        return true;
    }
    name.len() > domain.len()
        && name.as_bytes()[name.len() - domain.len() - 1] == b'.'
        && name.as_bytes()[name.len() - domain.len()..].eq_ignore_ascii_case(domain.as_bytes())
}

fn synthetic_record(rtype: ResourceType, data: RecordData, ttl: u32) -> ResourceRecord {
    ResourceRecord {
        name: DomainName::root(),
        rtype,
        class: Class::IN,
        ttl,
        data,
        raw: None,
    }
}

fn literal_result(data: RecordData, rtype: ResourceType) -> Arc<LookupResult> {
    Arc::new(LookupResult {
        name: DomainName::root(),
        records: vec![synthetic_record(rtype, data, 0)],
        security_status: None,
    })
}

// RFC 2308, 5 - Caching Negative Answers
// When the authoritative server creates this record its TTL is taken from the minimum of the SOA.MINIMUM field and
// SOA's TTL.
// Negative responses without SOA records SHOULD NOT be cached as there is no way to prevent the negative responses
// looping forever between a pair of servers even with a short TTL.
// `ttl_limit` is how long what led to the negative answer is valid, such as the aliases that led to it.
fn add_negative_answer(
    entry: &mut CacheEntry,
    rtype: ResourceType,
    response: &Message,
    ttl_limit: Option<u32>,
    now: Instant,
) {
    if rtype == ResourceType::ANY || entry.has_record_of_type(rtype) {
        return;
    }
    let soa_ttl = response.authorities.iter().find_map(|record| match &record.data {
        RecordData::Soa(soa) => Some(record.ttl.min(soa.minimum)),
        _ => None,
    });
    if let Some(ttl) = soa_ttl {
        let ttl = ttl_limit.map_or(ttl, |limit| ttl.min(limit));
        entry.add_negative_answer(rtype, now + Duration::from_secs(u64::from(ttl)));
    }
}

// How long the aliases among the records stay valid, if there are any.
fn alias_ttl(records: &[ResourceRecord]) -> Option<u32> {
    records
        .iter()
        .filter(|record| record.rtype == ResourceType::CNAME)
        .map(|record| record.ttl)
        .min()
}
