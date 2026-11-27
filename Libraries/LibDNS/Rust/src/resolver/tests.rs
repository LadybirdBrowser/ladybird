/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::message::Header;
use std::cell::Cell;
use std::rc::Rc;

const A_ADDRESS: Ipv4Addr = Ipv4Addr::new(192, 0, 2, 1);

// Answers a query with one fixed A and one fixed AAAA record, echoing the question.
fn respond(query: &[u8], mutate: impl FnOnce(&mut Message)) -> Vec<u8> {
    let query = Message::from_wire(query).expect("query parses");
    let question = query.questions[0].clone();
    let mut options = Options::default();
    options.set_is_question(false);
    options.set_recursion_available(true);
    let mut response = Message {
        header: Header {
            id: query.header.id,
            options,
            question_count: 1,
            ..Header::default()
        },
        questions: vec![question.clone()],
        ..Message::default()
    };
    for (rtype, data) in [
        (ResourceType::A, RecordData::A(A_ADDRESS)),
        (ResourceType::AAAA, RecordData::Aaaa(Ipv6Addr::LOCALHOST)),
    ] {
        response.answers.push(ResourceRecord {
            name: question.name.clone(),
            rtype,
            class: Class::IN,
            ttl: 300,
            data,
            raw: None,
        });
    }
    mutate(&mut response);
    response.header.answer_count = u16::try_from(response.answers.len()).unwrap();
    response.to_wire().unwrap()
}

fn frame(message: &[u8]) -> Vec<u8> {
    let mut framed = u16::try_from(message.len()).unwrap().to_be_bytes().to_vec();
    framed.extend_from_slice(message);
    framed
}

fn unframe(bytes: &[u8]) -> &[u8] {
    &bytes[2..]
}

#[derive(Debug)]
struct Connect {
    connection: ConnectionId,
    kind: ConnectKind,
    address: SocketAddr,
    server_name: String,
}

struct Harness {
    resolver: Resolver,
    now: Instant,
    ipv6_reachable: Rc<Cell<bool>>,
    done: HashMap<LookupId, Result<Arc<LookupResult>, Error>>,
    connects: Vec<Connect>,
    writes: Vec<(ConnectionId, Vec<u8>)>,
    closes: Vec<ConnectionId>,
    system: Vec<(SystemRequestId, String, AddressFamily)>,
    cancelled: Vec<SystemRequestId>,
}

impl Harness {
    fn new(server: Option<ServerConfig>) -> Self {
        let ipv6_reachable = Rc::new(Cell::new(true));
        let reachable = ipv6_reachable.clone();
        let mut next_id = 0u16;
        let mut resolver = Resolver::new(
            Box::new(move || reachable.get()),
            Box::new(move || {
                next_id = next_id.wrapping_add(1);
                Some(next_id)
            }),
        );
        let now = Instant::now();
        resolver.set_server(server, now);
        let mut harness = Self {
            resolver,
            now,
            ipv6_reachable,
            done: HashMap::new(),
            connects: Vec::new(),
            writes: Vec::new(),
            closes: Vec::new(),
            system: Vec::new(),
            cancelled: Vec::new(),
        };
        harness.drain();
        harness
    }

    fn udp() -> Self {
        Self::new(Some(ServerConfig {
            host: "127.0.0.1".to_owned(),
            port: 53,
            transport: ServerTransport::Udp,
        }))
    }

    fn tls(host: &str) -> Self {
        Self::new(Some(ServerConfig {
            host: host.to_owned(),
            port: 853,
            transport: ServerTransport::Tls,
        }))
    }

    fn drain(&mut self) {
        while let Some(action) = self.resolver.take_action() {
            match action {
                Action::Connect {
                    connection,
                    kind,
                    address,
                    server_name,
                } => self.connects.push(Connect {
                    connection,
                    kind,
                    address,
                    server_name,
                }),
                Action::Write { connection, bytes } => self.writes.push((connection, bytes)),
                Action::Close { connection } => self.closes.push(connection),
                Action::SystemResolve { request, name, family } => self.system.push((request, name, family)),
                Action::CancelSystemResolve { request } => self.cancelled.push(request),
                Action::LookupDone { lookup, result } => {
                    assert!(
                        self.done.insert(lookup, result).is_none(),
                        "lookup {lookup} settled twice"
                    );
                }
            }
        }
    }

    fn lookup(&mut self, name: &str) -> LookupId {
        self.lookup_with(name, &[ResourceType::A, ResourceType::AAAA], LookupOptions::default())
    }

    fn lookup_with(&mut self, name: &str, types: &[ResourceType], options: LookupOptions) -> LookupId {
        let id = self.resolver.lookup(name, Class::IN, types, options, self.now);
        self.drain();
        id
    }

    fn connected(&mut self, connection: ConnectionId) {
        self.resolver.on_connected(connection, self.now);
        self.drain();
    }

    fn receive(&mut self, connection: ConnectionId, bytes: &[u8]) {
        self.resolver.on_received(connection, bytes, self.now);
        self.drain();
    }

    fn advance(&mut self, by: Duration) {
        self.now += by;
        self.resolver.on_tick(self.now);
        self.drain();
    }

    fn system_resolved(&mut self, request: SystemRequestId, result: Result<Vec<IpAddr>, String>) {
        self.resolver.on_system_resolved(request, result, self.now);
        self.drain();
    }

    fn take_writes(&mut self) -> Vec<(ConnectionId, Vec<u8>)> {
        std::mem::take(&mut self.writes)
    }

    fn result(&self, lookup: LookupId) -> &Result<Arc<LookupResult>, Error> {
        self.done
            .get(&lookup)
            .unwrap_or_else(|| panic!("lookup {lookup} has not settled"))
    }

    fn addresses(&self, lookup: LookupId) -> Vec<IpAddr> {
        self.result(lookup).as_ref().expect("lookup succeeded").addresses()
    }

    fn error(&self, lookup: LookupId) -> String {
        self.result(lookup).as_ref().expect_err("lookup failed").to_string()
    }

    // Answers every query written so far over UDP.
    fn answer_udp(&mut self) {
        for (connection, query) in self.take_writes() {
            let response = respond(&query, |_| {});
            self.receive(connection, &response);
        }
    }
}

#[test]
fn udp_lookup_resolves_and_is_cached() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup("example.com");
    assert_eq!(harness.connects.len(), 1);
    assert_eq!(harness.connects[0].kind, ConnectKind::Udp);
    assert_eq!(harness.connects[0].address, "127.0.0.1:53".parse().unwrap());
    let connection = harness.connects[0].connection;

    assert!(harness.writes.is_empty());
    harness.connected(connection);
    assert_eq!(harness.writes.len(), 2);
    harness.answer_udp();
    assert_eq!(
        harness.addresses(lookup),
        vec![IpAddr::V4(A_ADDRESS), IpAddr::V6(Ipv6Addr::LOCALHOST)]
    );

    let again = harness.lookup("example.com");
    assert!(harness.writes.is_empty());
    assert_eq!(harness.addresses(again).len(), 2);
}

#[test]
fn tls_lookups_wait_for_the_handshake() {
    let mut harness = Harness::tls("9.9.9.9");
    let first = harness.lookup("example.com");
    let second = harness.lookup("example.org");
    assert_eq!(harness.connects.len(), 1);
    assert_eq!(harness.connects[0].kind, ConnectKind::Tls);
    assert_eq!(harness.connects[0].server_name, "9.9.9.9");
    assert!(harness.writes.is_empty());
    assert!(harness.done.is_empty());

    let connection = harness.connects[0].connection;
    harness.connected(connection);
    let writes = harness.take_writes();
    assert_eq!(writes.len(), 4);
    for (connection, query) in writes {
        let response = respond(unframe(&query), |_| {});
        harness.receive(connection, &frame(&response));
    }
    assert_eq!(harness.addresses(first).len(), 2);
    assert_eq!(harness.addresses(second).len(), 2);
}

#[test]
fn tls_server_that_never_finishes_the_handshake_fails_lookups_without_blocking() {
    let mut harness = Harness::tls("9.9.9.9");
    let lookup = harness.lookup("example.com");
    let connection = harness.connects[0].connection;
    assert_eq!(harness.resolver.next_deadline(), Some(harness.now + CONNECT_TIMEOUT));

    harness.advance(CONNECT_TIMEOUT - Duration::from_millis(1));
    assert!(harness.done.is_empty());
    harness.advance(Duration::from_millis(1));
    assert_eq!(harness.error(lookup), "Connecting to the DNS server timed out");
    assert_eq!(harness.closes, vec![connection]);

    harness.lookup("example.org");
    assert_eq!(harness.connects.len(), 2);
}

#[test]
fn tls_server_given_by_name_is_looked_up_with_the_system_resolver() {
    let mut harness = Harness::tls("dns.quad9.net");
    let lookup = harness.lookup("example.com");
    assert!(harness.connects.is_empty());
    let requests = std::mem::take(&mut harness.system);
    assert_eq!(requests.len(), 2);
    assert!(requests.iter().all(|(_, name, _)| name == "dns.quad9.net"));

    for (request, _, family) in requests {
        let address = match family {
            AddressFamily::V4 => IpAddr::V4(Ipv4Addr::new(9, 9, 9, 9)),
            AddressFamily::V6 => IpAddr::V6("2620:fe::fe".parse().unwrap()),
        };
        harness.system_resolved(request, Ok(vec![address]));
    }
    assert_eq!(harness.connects.len(), 1);
    assert_eq!(harness.connects[0].address, "9.9.9.9:853".parse().unwrap());
    assert_eq!(harness.connects[0].server_name, "dns.quad9.net");

    let connection = harness.connects[0].connection;
    harness.connected(connection);
    for (connection, query) in harness.take_writes() {
        let response = respond(unframe(&query), |_| {});
        harness.receive(connection, &frame(&response));
    }
    assert_eq!(harness.addresses(lookup).len(), 2);
}

#[test]
fn tls_server_whose_name_does_not_resolve_fails_lookups() {
    let mut harness = Harness::tls("dns.invalid");
    let lookup = harness.lookup("example.com");
    for (request, _, _) in std::mem::take(&mut harness.system) {
        harness.system_resolved(request, Err("no such name".to_owned()));
    }
    assert_eq!(
        harness.error(lookup),
        "DNS server connection failed: Failed to resolve DNS server hostname"
    );
    assert!(harness.connects.is_empty());
}

#[test]
fn stream_responses_can_arrive_in_pieces() {
    let mut harness = Harness::tls("9.9.9.9");
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let connection = harness.connects[0].connection;
    harness.connected(connection);
    let (_, query) = harness.take_writes().remove(0);
    let response = frame(&respond(unframe(&query), |_| {}));
    for byte in &response[..response.len() - 1] {
        harness.receive(connection, std::slice::from_ref(byte));
        assert!(harness.done.is_empty());
    }
    harness.receive(connection, &response[response.len() - 1..]);
    assert_eq!(harness.addresses(lookup), vec![IpAddr::V4(A_ADDRESS)]);
}

#[test]
fn closed_tls_connection_is_replaced_and_questions_are_asked_again() {
    let mut harness = Harness::tls("9.9.9.9");
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let first = harness.connects[0].connection;
    harness.connected(first);
    assert_eq!(harness.take_writes().len(), 1);

    // DoT servers close idle connections. The question in flight goes out again on the next one.
    harness.resolver.on_eof(first, harness.now);
    harness.drain();
    assert_eq!(harness.closes, vec![first]);
    assert!(harness.done.is_empty());

    harness.advance(QUERY_RETRY_INTERVAL);
    assert_eq!(harness.connects.len(), 2);
    let second = harness.connects[1].connection;
    harness.connected(second);
    let writes = harness.take_writes();
    assert_eq!(writes.len(), 1);
    assert_eq!(writes[0].0, second);
    let response = respond(unframe(&writes[0].1), |_| {});
    harness.receive(second, &frame(&response));
    assert!(harness.result(lookup).is_ok());
}

#[test]
fn udp_connection_whose_send_fails_is_replaced() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let first = harness.connects[0].connection;
    harness.connected(first);
    assert_eq!(harness.take_writes().len(), 1);

    // A connected UDP socket keeps the source address it got when it connected, so once the network it was opened on
    // is gone, every send on it fails. That's what the socket owner reports here.
    harness
        .resolver
        .on_connect_failed(first, "Network is unreachable (errno=101)".to_owned(), harness.now);
    harness.drain();
    assert_eq!(harness.closes, vec![first]);
    assert!(harness.done.is_empty());

    // The next lookup goes out on a new connection.
    let later = harness.lookup_with("ladybird.org", &[ResourceType::A], LookupOptions::default());
    assert_eq!(harness.connects.len(), 2);
    let second = harness.connects[1].connection;
    assert_ne!(second, first);
    harness.connected(second);
    let writes = harness.take_writes();
    assert_eq!(writes.len(), 1);
    assert_eq!(writes[0].0, second);
    let response = respond(&writes[0].1, |_| {});
    harness.receive(second, &response);
    assert!(harness.result(later).is_ok());

    // And the question that was in flight goes out again on it at its next retry.
    harness.advance(QUERY_RETRY_INTERVAL);
    let writes = harness.take_writes();
    assert_eq!(writes.len(), 1);
    assert_eq!(writes[0].0, second);
    let response = respond(&writes[0].1, |_| {});
    harness.receive(second, &response);
    assert!(harness.result(lookup).is_ok());
}

#[test]
fn udp_connection_that_answers_nothing_is_replaced_before_a_retry() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let first = harness.connects[0].connection;
    harness.connected(first);
    assert_eq!(harness.take_writes().len(), 1);

    // Sending works, but nothing comes back, as when the network changed under a socket that keeps its old address.
    harness.advance(QUERY_RETRY_INTERVAL);
    assert_eq!(harness.connects.len(), 2);
    let second = harness.connects[1].connection;
    harness.connected(second);
    let writes = harness.take_writes();
    assert_eq!(writes.len(), 1);
    assert_eq!(writes[0].0, second);
    let response = respond(&writes[0].1, |_| {});
    harness.receive(second, &response);
    assert!(harness.result(lookup).is_ok());

    // The old connection closes once no question is waiting for an answer.
    assert!(harness.closes.is_empty());
    harness.advance(QUERY_RETRY_INTERVAL);
    assert_eq!(harness.closes, vec![first]);
}

#[test]
fn replaced_udp_connection_still_takes_a_late_answer() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let first = harness.connects[0].connection;
    harness.connected(first);
    let (_, query) = harness.take_writes().remove(0);

    // A slow server answers on the first connection after the retry already went out on a second one.
    harness.advance(QUERY_RETRY_INTERVAL);
    assert_eq!(harness.connects.len(), 2);
    let response = respond(&query, |_| {});
    harness.receive(first, &response);
    assert!(harness.result(lookup).is_ok());
}

#[test]
fn udp_connection_that_answers_is_kept_when_a_query_is_retried() {
    let mut harness = Harness::udp();
    let answered = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let unanswered = harness.lookup_with("ladybird.org", &[ResourceType::A], LookupOptions::default());
    let connection = harness.connects[0].connection;
    harness.connected(connection);
    let writes = harness.take_writes();
    assert_eq!(writes.len(), 2);
    let response = respond(&writes[0].1, |_| {});
    harness.receive(connection, &response);
    assert!(harness.result(answered).is_ok());

    // The connection answered after the other query went out, so that query was just lost, and goes out again on it.
    harness.advance(QUERY_RETRY_INTERVAL);
    assert!(harness.closes.is_empty());
    assert_eq!(harness.connects.len(), 1);
    let writes = harness.take_writes();
    assert_eq!(writes.len(), 1);
    assert_eq!(writes[0].0, connection);
    let response = respond(&writes[0].1, |_| {});
    harness.receive(connection, &response);
    assert!(harness.result(unanswered).is_ok());
}

#[test]
fn malformed_stream_message_leaves_the_next_one_alone() {
    let mut harness = Harness::tls("9.9.9.9");
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let connection = harness.connects[0].connection;
    harness.connected(connection);
    let (_, query) = harness.take_writes().remove(0);

    let mut bytes = frame(&[1, 2, 3]);
    bytes.extend(frame(&respond(unframe(&query), |_| {})));
    harness.receive(connection, &bytes);
    assert!(harness.closes.is_empty());
    assert!(harness.result(lookup).is_ok());
}

#[test]
fn unanswered_queries_are_retried_then_time_out() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    harness.connected(harness.connects[0].connection);
    assert_eq!(harness.take_writes().len(), 1);
    // Each retry goes out on a new connection, since the last one answered nothing.
    for attempt in 1..QUERY_MAX_ATTEMPTS {
        harness.advance(QUERY_RETRY_INTERVAL);
        assert_eq!(harness.connects.len(), attempt + 1);
        harness.connected(harness.connects[attempt].connection);
        assert_eq!(harness.take_writes().len(), 1);
    }
    assert!(harness.done.is_empty());
    harness.advance(QUERY_RETRY_INTERVAL);
    assert_eq!(harness.error(lookup), "DNS lookup timed out");
    assert_eq!(harness.resolver.next_deadline(), None);
}

#[test]
fn truncated_udp_response_is_retried_over_tcp() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let udp = harness.connects[0].connection;
    harness.connected(udp);
    let (_, query) = harness.take_writes().remove(0);
    let truncated = respond(&query, |response| {
        response.header.options.set_is_truncated(true);
        response.answers.clear();
    });
    harness.receive(udp, &truncated);

    assert_eq!(harness.connects.len(), 2);
    assert_eq!(harness.connects[1].kind, ConnectKind::Tcp);
    assert_eq!(harness.connects[1].address, "127.0.0.1:53".parse().unwrap());
    let tcp = harness.connects[1].connection;
    harness.connected(tcp);
    let (connection, query) = harness.take_writes().remove(0);
    assert_eq!(connection, tcp);
    harness.receive(tcp, &frame(&respond(unframe(&query), |_| {})));
    assert!(harness.result(lookup).is_ok());
}

#[test]
fn truncated_response_over_a_stream_fails() {
    let mut harness = Harness::tls("9.9.9.9");
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let connection = harness.connects[0].connection;
    harness.connected(connection);
    let (_, query) = harness.take_writes().remove(0);
    let truncated = respond(unframe(&query), |response| {
        response.header.options.set_is_truncated(true);
    });
    harness.receive(connection, &frame(&truncated));
    assert_eq!(harness.error(lookup), "DNS response was truncated");
}

#[test]
fn responses_that_do_not_echo_the_question_are_ignored() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let connection = harness.connects[0].connection;
    harness.connected(connection);
    let (_, query) = harness.take_writes().remove(0);
    let wrong = respond(&query, |response| {
        response.questions[0].name = DomainName::from_string("evil.example");
    });
    harness.receive(connection, &wrong);
    assert!(harness.done.is_empty());
    harness.receive(connection, &respond(&query, |_| {}));
    assert!(harness.result(lookup).is_ok());
}

#[test]
fn unrelated_answers_are_not_cached() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let connection = harness.connects[0].connection;
    harness.connected(connection);
    let (_, query) = harness.take_writes().remove(0);
    let response = respond(&query, |response| {
        for answer in &mut response.answers {
            answer.name = DomainName::from_string("victim.example");
        }
    });
    harness.receive(connection, &response);
    assert!(harness.result(lookup).as_ref().unwrap().records.is_empty());

    harness.lookup_with("victim.example", &[ResourceType::A], LookupOptions::default());
    assert_eq!(harness.take_writes().len(), 1);
}

#[test]
fn concurrent_lookups_for_one_name_share_one_query() {
    let mut harness = Harness::udp();
    let lookups: Vec<_> = (0..3).map(|_| harness.lookup("example.com")).collect();
    harness.connected(harness.connects[0].connection);
    assert_eq!(harness.writes.len(), 2);
    harness.answer_udp();
    for lookup in lookups {
        assert_eq!(harness.addresses(lookup).len(), 2);
    }
}

#[test]
fn server_errors_fail_the_lookup() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let connection = harness.connects[0].connection;
    harness.connected(connection);
    let (_, query) = harness.take_writes().remove(0);
    harness.receive(
        connection,
        &respond(&query, |response| {
            response.header.options.set_response_code(ResponseCode::SERVER_FAILURE);
        }),
    );
    assert_eq!(harness.error(lookup), "DNS server returned an error");
}

#[test]
fn configured_server_failure_does_not_fall_back_to_the_system_resolver() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup("example.com");
    harness
        .resolver
        .on_connect_failed(harness.connects[0].connection, "refused".to_owned(), harness.now);
    harness.drain();
    assert_eq!(harness.error(lookup), "DNS server connection failed: refused");
    assert!(harness.system.is_empty());
}

#[test]
fn literals_and_loopback_names_resolve_without_a_socket() {
    let mut harness = Harness::udp();
    let v4 = harness.lookup("192.0.2.7");
    let v6 = harness.lookup("::1");
    let localhost = harness.lookup("LocalHost.");
    let sub = harness.lookup("app.localhost");
    harness.resolver.set_loopback_domains(vec!["test".to_owned()]);
    let test = harness.lookup("web-platform.test");
    assert!(harness.connects.is_empty());
    assert_eq!(harness.addresses(v4), vec![IpAddr::V4(Ipv4Addr::new(192, 0, 2, 7))]);
    assert_eq!(harness.addresses(v6), vec![IpAddr::V6(Ipv6Addr::LOCALHOST)]);
    for lookup in [localhost, sub, test] {
        assert_eq!(
            harness.addresses(lookup),
            vec![IpAddr::V4(Ipv4Addr::LOCALHOST), IpAddr::V6(Ipv6Addr::LOCALHOST)]
        );
    }
}

#[test]
fn bracketed_ipv6_literals_resolve_without_a_socket() {
    let mut harness = Harness::new(None);
    let lookup = harness.lookup("[::ffff:7f00:1]");
    assert!(harness.system.is_empty());
    assert_eq!(
        harness.addresses(lookup),
        vec![IpAddr::V6("::ffff:127.0.0.1".parse().unwrap())]
    );
}

#[test]
fn names_that_must_not_go_upstream_are_rejected() {
    let mut harness = Harness::udp();
    let onion = harness.lookup("duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion");
    let empty_label = harness.lookup("example..");
    let long_label = harness.lookup(&format!("{}.com", "a".repeat(64)));
    assert_eq!(harness.error(onion), "Refusing to resolve a .onion name");
    assert_eq!(harness.error(empty_label), "Invalid domain name");
    assert_eq!(harness.error(long_label), "Invalid domain name");
    assert!(harness.connects.is_empty());
}

#[test]
fn reset_fails_whats_in_flight_and_forgets_the_cache() {
    let mut harness = Harness::udp();
    let cached = harness.lookup("example.com");
    harness.connected(harness.connects[0].connection);
    harness.answer_udp();
    assert!(harness.result(cached).is_ok());

    let in_flight = harness.lookup("example.org");
    harness.resolver.reset_connection(harness.now);
    harness.drain();
    assert_eq!(harness.error(in_flight), "DNS connection was reset");
    assert_eq!(harness.resolver.cached_entry_count(), 0);
}

#[test]
fn expired_addresses_are_not_mistaken_for_a_negative_answer() {
    let mut harness = Harness::udp();
    let first = harness.lookup("example.com");
    harness.connected(harness.connects[0].connection);
    harness.answer_udp();
    assert!(harness.result(first).is_ok());

    harness.now += Duration::from_secs(301);
    harness.lookup("example.com");
    assert_eq!(harness.take_writes().len(), 2);
}

// ---- The system resolver ---------------------------------------------------------------------------------------

fn system_requests(harness: &mut Harness) -> (SystemRequestId, Option<SystemRequestId>) {
    let requests = std::mem::take(&mut harness.system);
    let a = requests
        .iter()
        .find(|(_, _, family)| *family == AddressFamily::V4)
        .expect("A side")
        .0;
    let aaaa = requests
        .iter()
        .find(|(_, _, family)| *family == AddressFamily::V6)
        .map(|request| request.0);
    (a, aaaa)
}

#[test]
fn system_resolver_waits_briefly_for_ipv6_after_ipv4() {
    let mut harness = Harness::new(None);
    let lookup = harness.lookup("example.com");
    let (a, aaaa) = system_requests(&mut harness);
    harness.system_resolved(a, Ok(vec![IpAddr::V4(A_ADDRESS)]));
    assert!(harness.done.is_empty());
    harness.system_resolved(aaaa.unwrap(), Ok(vec![IpAddr::V6(Ipv6Addr::LOCALHOST)]));
    assert_eq!(harness.addresses(lookup).len(), 2);
}

#[test]
fn system_resolver_goes_ahead_with_ipv4_when_ipv6_takes_too_long() {
    let mut harness = Harness::new(None);
    let lookup = harness.lookup("example.com");
    let (a, aaaa) = system_requests(&mut harness);
    harness.system_resolved(a, Ok(vec![IpAddr::V4(A_ADDRESS)]));
    harness.advance(IPV6_RESOLUTION_DELAY);
    assert_eq!(harness.addresses(lookup), vec![IpAddr::V4(A_ADDRESS)]);

    // The late side still lands in the cache.
    harness.system_resolved(aaaa.unwrap(), Ok(vec![IpAddr::V6(Ipv6Addr::LOCALHOST)]));
    let again = harness.lookup("example.com");
    assert!(harness.system.is_empty());
    assert_eq!(harness.addresses(again).len(), 2);
}

#[test]
fn system_resolver_waits_for_ipv4_after_ipv6() {
    let mut harness = Harness::new(None);
    let lookup = harness.lookup("example.com");
    let (a, aaaa) = system_requests(&mut harness);
    harness.system_resolved(aaaa.unwrap(), Ok(vec![IpAddr::V6(Ipv6Addr::LOCALHOST)]));
    harness.advance(IPV6_RESOLUTION_DELAY);
    assert!(harness.done.is_empty());
    harness.system_resolved(a, Ok(vec![IpAddr::V4(A_ADDRESS)]));
    assert_eq!(harness.addresses(lookup).len(), 2);
}

#[test]
fn system_resolver_goes_ahead_with_ipv6_when_ipv4_takes_too_long() {
    let mut harness = Harness::new(None);
    let lookup = harness.lookup("example.com");
    let (_, aaaa) = system_requests(&mut harness);
    harness.system_resolved(aaaa.unwrap(), Ok(vec![IpAddr::V6(Ipv6Addr::LOCALHOST)]));
    harness.advance(IPV4_RESOLUTION_DELAY);
    assert_eq!(harness.addresses(lookup), vec![IpAddr::V6(Ipv6Addr::LOCALHOST)]);
}

#[test]
fn system_resolver_skips_ipv6_when_ipv6_is_unreachable() {
    let mut harness = Harness::new(None);
    harness.ipv6_reachable.set(false);
    let lookup = harness.lookup("example.com");
    let (a, aaaa) = system_requests(&mut harness);
    assert_eq!(aaaa, None);
    harness.system_resolved(a, Ok(vec![IpAddr::V4(A_ADDRESS)]));
    assert_eq!(harness.addresses(lookup), vec![IpAddr::V4(A_ADDRESS)]);

    // On request, and for local names, the AAAA side goes ahead anyway.
    let options = LookupOptions {
        query_aaaa_without_global_ipv6: true,
        ..LookupOptions::default()
    };
    harness.lookup_with("example.org", &[ResourceType::A, ResourceType::AAAA], options);
    assert!(system_requests(&mut harness).1.is_some());
    harness.lookup("printer.local");
    assert!(system_requests(&mut harness).1.is_some());
}

#[test]
fn system_resolver_looks_up_ipv6_again_once_ipv6_is_reachable() {
    let mut harness = Harness::new(None);
    harness.ipv6_reachable.set(false);
    harness.lookup("example.com");
    let (a, _) = system_requests(&mut harness);
    harness.system_resolved(a, Ok(vec![IpAddr::V4(A_ADDRESS)]));

    harness.ipv6_reachable.set(true);
    harness.now += IPV6_REACHABILITY_CHECK_INTERVAL;
    harness.lookup("example.com");
    let (_, aaaa) = system_requests(&mut harness);
    assert!(aaaa.is_some());
}

#[test]
fn system_resolver_failure_fails_the_lookup() {
    let mut harness = Harness::new(None);
    let lookup = harness.lookup("example.com");
    let (a, aaaa) = system_requests(&mut harness);
    harness.system_resolved(a, Err("nope".to_owned()));
    harness.system_resolved(aaaa.unwrap(), Err("nope".to_owned()));
    assert_eq!(harness.error(lookup), "Could not resolve to IPv4 or IPv6 address");
}

#[test]
fn local_names_go_to_the_system_resolver_even_with_a_server() {
    let mut harness = Harness::udp();
    harness.lookup("printer.local");
    assert!(harness.connects.is_empty());
    assert_eq!(harness.system.len(), 2);
}

#[test]
fn validating_without_a_server_fails() {
    let mut harness = Harness::new(None);
    let options = LookupOptions {
        validate_dnssec_locally: true,
        ..LookupOptions::default()
    };
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], options);
    assert_eq!(harness.error(lookup), "No connection available to validate DNSSEC");
}

// ---- DNSSEC ----------------------------------------------------------------------------------------------------

// Answers every query the resolver sends from the signed test zones, until nothing more goes out.
fn serve_signed_zones(harness: &mut Harness, server: &mut crate::dnssec::tests::Server) {
    loop {
        let writes = harness.take_writes();
        if writes.is_empty() {
            return;
        }
        for (connection, query) in writes {
            let query = Message::from_wire(&query).unwrap();
            let mut response = server.answer(&query.questions[0]);
            response.header.id = query.header.id;
            harness.receive(connection, &response.to_wire().unwrap());
        }
    }
}

fn dnssec_harness(server: &crate::dnssec::tests::Server) -> Harness {
    let mut harness = Harness::udp();
    harness.resolver.set_trust_anchors(vec![server.root_ds()]);
    harness
        .resolver
        .set_wall_clock(harness.now, u64::from(crate::dnssec::tests::NOW));
    harness
}

const VALIDATE: LookupOptions = LookupOptions {
    validate_dnssec_locally: true,
    query_aaaa_without_global_ipv6: false,
};

#[test]
fn validated_lookup_is_secure() {
    let mut server = crate::dnssec::tests::make_server(false);
    server.start();
    let mut harness = dnssec_harness(&server);
    let lookup = harness.lookup_with("www.example", &[ResourceType::A], VALIDATE);
    harness.connected(harness.connects[0].connection);
    serve_signed_zones(&mut harness, &mut server);

    let result = harness.result(lookup).as_ref().unwrap();
    assert_eq!(result.security_status, Some(SecurityStatus::Secure));
    assert_eq!(result.addresses(), vec![IpAddr::V4(Ipv4Addr::new(192, 0, 2, 1))]);

    let again = harness.lookup_with("www.example", &[ResourceType::A], VALIDATE);
    assert!(harness.writes.is_empty());
    assert_eq!(
        harness.result(again).as_ref().unwrap().security_status,
        Some(SecurityStatus::Secure)
    );
}

#[test]
fn validated_lookup_in_an_unsigned_zone_is_insecure() {
    let mut server = crate::dnssec::tests::make_server(true);
    server.start();
    let mut harness = dnssec_harness(&server);
    let lookup = harness.lookup_with("host.unsigned.example", &[ResourceType::A], VALIDATE);
    harness.connected(harness.connects[0].connection);
    serve_signed_zones(&mut harness, &mut server);
    assert_eq!(
        harness.result(lookup).as_ref().unwrap().security_status,
        Some(SecurityStatus::Insecure)
    );
}

#[test]
fn validated_lookup_with_the_wrong_trust_anchor_fails() {
    let mut server = crate::dnssec::tests::make_server(false);
    server.start();
    let mut other = crate::dnssec::tests::make_server(false);
    other.start();
    let mut harness = dnssec_harness(&other);
    let lookup = harness.lookup_with("www.example", &[ResourceType::A], VALIDATE);
    harness.connected(harness.connects[0].connection);
    serve_signed_zones(&mut harness, &mut server);
    assert_eq!(harness.error(lookup), "DNSSEC validation failed");
}

#[test]
fn validated_lookup_whose_fetch_times_out_fails_with_the_timeout() {
    let mut server = crate::dnssec::tests::make_server(false);
    server.start();
    let mut harness = dnssec_harness(&server);
    let lookup = harness.lookup_with("www.example", &[ResourceType::A], VALIDATE);
    harness.connected(harness.connects[0].connection);

    // The server answers the lookup's own query, and then stops answering, as when the network changed under it.
    let (connection, query) = harness.take_writes().remove(0);
    let query = Message::from_wire(&query).unwrap();
    let mut response = server.answer(&query.questions[0]);
    response.header.id = query.header.id;
    harness.receive(connection, &response.to_wire().unwrap());
    let mut connected = harness.connects.len();
    for _ in 0..20 {
        harness.take_writes();
        harness.advance(QUERY_RETRY_INTERVAL);
        let new: Vec<_> = harness.connects[connected..]
            .iter()
            .map(|connect| connect.connection)
            .collect();
        connected = harness.connects.len();
        for connection in new {
            harness.connected(connection);
        }
    }
    assert_eq!(harness.error(lookup), "DNS lookup timed out");
}

#[test]
fn unvalidated_cache_entries_do_not_serve_validated_lookups() {
    let mut server = crate::dnssec::tests::make_server(false);
    server.start();
    let mut harness = dnssec_harness(&server);
    harness.lookup_with("www.example", &[ResourceType::A], LookupOptions::default());
    harness.connected(harness.connects[0].connection);
    serve_signed_zones(&mut harness, &mut server);

    // The unvalidated answer is cached, and serves unvalidated lookups.
    let cached = harness.lookup_with("www.example", &[ResourceType::A], LookupOptions::default());
    assert!(harness.writes.is_empty());
    assert_eq!(harness.result(cached).as_ref().unwrap().security_status, None);

    let lookup = harness.lookup_with("www.example", &[ResourceType::A], VALIDATE);
    assert!(!harness.writes.is_empty());
    serve_signed_zones(&mut harness, &mut server);
    assert_eq!(
        harness.result(lookup).as_ref().unwrap().security_status,
        Some(SecurityStatus::Secure)
    );
}

#[test]
fn lookup_for_no_types_settles_at_once() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("example.com", &[], LookupOptions::default());
    assert!(harness.result(lookup).as_ref().unwrap().records.is_empty());
    assert!(harness.connects.is_empty());
}

#[test]
fn server_name_looked_up_with_the_system_resolver_stays_out_of_the_cache() {
    let mut harness = Harness::tls("dns.quad9.net");
    harness.lookup("example.com");
    for (request, _, family) in std::mem::take(&mut harness.system) {
        let address = match family {
            AddressFamily::V4 => IpAddr::V4(Ipv4Addr::new(9, 9, 9, 9)),
            AddressFamily::V6 => IpAddr::V6("2620:fe::fe".parse().unwrap()),
        };
        harness.system_resolved(request, Ok(vec![address]));
    }
    assert_eq!(harness.connects.len(), 1);
    harness.connected(harness.connects[0].connection);
    harness.take_writes();

    // Looking up the server's own name goes to the server, rather than coming from what the system resolver said.
    let lookup = harness.lookup_with("dns.quad9.net", &[ResourceType::A], LookupOptions::default());
    assert!(!harness.done.contains_key(&lookup));
    assert_eq!(harness.take_writes().len(), 1);
}

#[test]
fn server_names_get_the_same_special_handling_as_other_names() {
    let mut harness = Harness::tls("localhost.");
    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    assert!(harness.system.is_empty());
    assert!(harness.connects[0].address.ip().is_loopback());

    let mut harness = Harness::tls("dnsresolverabcdefghijklmnopqrstuvwxyz234567abcdefghijklmn.onion");
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    assert!(harness.system.is_empty());
    assert!(harness.connects.is_empty());
    assert!(harness.result(lookup).is_err());
}

#[test]
fn lookup_fails_when_no_random_query_id_can_be_had() {
    let mut resolver = Resolver::new(Box::new(|| true), Box::new(|| None));
    let now = Instant::now();
    resolver.set_server(
        Some(ServerConfig {
            host: "127.0.0.1".to_owned(),
            port: 53,
            transport: ServerTransport::Udp,
        }),
        now,
    );
    let lookup = resolver.lookup(
        "example.com",
        Class::IN,
        &[ResourceType::A],
        LookupOptions::default(),
        now,
    );
    let mut failure = None;
    while let Some(action) = resolver.take_action() {
        if let Action::LookupDone { lookup: done, result } = action {
            assert_eq!(done, lookup);
            failure = result.err();
        }
    }
    assert_eq!(failure.unwrap().to_string(), "Could not pick a random DNS query ID");
}

#[test]
fn port_zero_is_the_default_port_of_the_transport() {
    for (transport, port) in [(ServerTransport::Udp, 53), (ServerTransport::Tls, 853)] {
        let mut harness = Harness::new(Some(ServerConfig {
            host: "192.0.2.53".to_owned(),
            port: 0,
            transport,
        }));
        harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
        assert_eq!(harness.connects[0].address.port(), port);
    }
}
#[test]
fn queries_for_names_that_differ_only_in_label_boundaries_are_kept_apart() {
    let mut harness = Harness::udp();
    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    harness.connected(harness.connects[0].connection);
    harness.take_writes();

    let dotted_label = DomainName {
        labels: vec![b"a.b".to_vec(), b"example".to_vec()],
    };
    let two_labels = DomainName::from_string("a.b.example");
    for (index, name) in [dotted_label, two_labels].iter().enumerate() {
        let waiter = QueryWaiter::Validation(index as u64);
        harness
            .resolver
            .query(name, ResourceType::DS, Class::IN, true, waiter, harness.now);
    }
    harness.drain();
    assert_eq!(harness.take_writes().len(), 2);
}

#[test]
fn zero_ttl_answers_are_not_served_from_the_cache() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let connection = harness.connects[0].connection;
    harness.connected(connection);
    let (_, query) = harness.take_writes().remove(0);
    let response = respond(&query, |response| {
        for answer in &mut response.answers {
            answer.ttl = 0;
        }
    });
    harness.receive(connection, &response);
    assert_eq!(harness.addresses(lookup), vec![IpAddr::V4(A_ADDRESS)]);

    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    assert_eq!(harness.take_writes().len(), 1);
}

#[test]
fn cache_hits_only_return_the_requested_types() {
    let mut harness = Harness::udp();
    let both = harness.lookup("example.com");
    harness.connected(harness.connects[0].connection);
    harness.answer_udp();
    assert_eq!(harness.addresses(both).len(), 2);

    let only_a = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    assert!(harness.writes.is_empty());
    assert_eq!(harness.addresses(only_a), vec![IpAddr::V4(A_ADDRESS)]);
}

#[test]
fn system_resolver_starts_over_once_a_grace_window_result_has_expired() {
    let mut harness = Harness::new(None);
    let first = harness.lookup("example.com");
    let (a, stalled_aaaa) = system_requests(&mut harness);
    harness.system_resolved(a, Ok(vec![IpAddr::V4(A_ADDRESS)]));
    harness.advance(IPV6_RESOLUTION_DELAY);
    assert_eq!(harness.addresses(first), vec![IpAddr::V4(A_ADDRESS)]);

    // The AAAA side never answers. While the A record is fresh, it serves.
    let joined = harness.lookup("example.com");
    assert!(harness.system.is_empty());
    assert_eq!(harness.addresses(joined), vec![IpAddr::V4(A_ADDRESS)]);

    harness.advance(Duration::from_secs(u64::from(SYSTEM_RESOLVER_SYNTHETIC_TTL_SECONDS)));
    let later = harness.lookup("example.com");
    let (a, aaaa) = system_requests(&mut harness);
    assert!(!harness.done.contains_key(&later));

    // The answer to the abandoned resolution changes nothing for the new one.
    harness.system_resolved(stalled_aaaa.unwrap(), Ok(vec![IpAddr::V6(Ipv6Addr::LOCALHOST)]));
    assert!(!harness.done.contains_key(&later));
    harness.system_resolved(a, Ok(vec![IpAddr::V4(A_ADDRESS)]));
    harness.system_resolved(aaaa.unwrap(), Ok(vec![]));
    assert_eq!(harness.addresses(later), vec![IpAddr::V4(A_ADDRESS)]);
}

fn soa(ttl: u32, minimum: u32) -> ResourceRecord {
    ResourceRecord {
        name: DomainName::from_string("example.com"),
        rtype: ResourceType::SOA,
        class: Class::IN,
        ttl,
        data: RecordData::Soa(crate::message::Soa {
            mname: DomainName::from_string("ns.example.com"),
            rname: DomainName::from_string("hostmaster.example.com"),
            minimum,
            ..crate::message::Soa::default()
        }),
        raw: None,
    }
}

// Answers A queries with an address that lives for 300 seconds, and AAAA queries with no data.
fn answer_without_aaaa(harness: &mut Harness, authority: Option<&ResourceRecord>) {
    for (connection, query) in harness.take_writes() {
        let response = respond(&query, |response| {
            let rtype = response.questions[0].rtype;
            response
                .answers
                .retain(|answer| answer.rtype == rtype && rtype == ResourceType::A);
            response.authorities.extend(authority.cloned());
            response.header.authority_count = u16::try_from(response.authorities.len()).unwrap();
        });
        harness.receive(connection, &response);
    }
}

#[test]
fn negative_answers_are_cached_for_as_long_as_the_soa_says() {
    let mut harness = Harness::udp();
    let first = harness.lookup("example.com");
    harness.connected(harness.connects[0].connection);
    answer_without_aaaa(&mut harness, Some(&soa(3600, 5)));
    assert_eq!(harness.addresses(first), vec![IpAddr::V4(A_ADDRESS)]);

    harness.lookup("example.com");
    assert!(harness.writes.is_empty());

    // The address is still fresh, but that there is none for IPv6 no longer holds.
    harness.now += Duration::from_secs(5);
    harness.lookup("example.com");
    assert_eq!(harness.take_writes().len(), 2);
    let only_a = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    assert!(harness.writes.is_empty());
    assert_eq!(harness.addresses(only_a), vec![IpAddr::V4(A_ADDRESS)]);
}

#[test]
fn negative_answers_without_an_soa_are_not_cached() {
    let mut harness = Harness::udp();
    harness.lookup("example.com");
    harness.connected(harness.connects[0].connection);
    answer_without_aaaa(&mut harness, None);

    harness.lookup("example.com");
    assert_eq!(harness.take_writes().len(), 2);
}

#[test]
fn negative_answers_behind_an_alias_last_no_longer_than_the_alias() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("www.example.com", &[ResourceType::AAAA], LookupOptions::default());
    harness.connected(harness.connects[0].connection);
    let (connection, query) = harness.take_writes().remove(0);
    let response = respond(&query, |response| {
        response.answers = vec![ResourceRecord {
            name: DomainName::from_string("www.example.com"),
            rtype: ResourceType::CNAME,
            class: Class::IN,
            ttl: 5,
            data: RecordData::Cname(DomainName::from_string("cdn.example.com")),
            raw: None,
        }];
        response.authorities.push(soa(300, 300));
        response.header.authority_count = 1;
    });
    harness.receive(connection, &response);
    assert!(harness.result(lookup).as_ref().unwrap().addresses().is_empty());

    harness.now += Duration::from_secs(5);
    harness.lookup_with("www.example.com", &[ResourceType::AAAA], LookupOptions::default());
    assert_eq!(harness.take_writes().len(), 1);
}

#[test]
fn system_resolver_only_looks_up_the_requested_address_families() {
    let mut harness = Harness::new(None);
    let only_aaaa = harness.lookup_with("example.com", &[ResourceType::AAAA], LookupOptions::default());
    let requests = std::mem::take(&mut harness.system);
    assert_eq!(requests.len(), 1);
    assert_eq!(requests[0].2, AddressFamily::V6);
    harness.system_resolved(requests[0].0, Ok(vec![IpAddr::V6(Ipv6Addr::LOCALHOST)]));
    assert_eq!(harness.addresses(only_aaaa), vec![IpAddr::V6(Ipv6Addr::LOCALHOST)]);

    let tlsa = harness.lookup_with("_443._tcp.example.com", &[ResourceType::TLSA], LookupOptions::default());
    assert!(harness.system.is_empty());
    assert_eq!(harness.error(tlsa), "The system resolver can only look up addresses");
}

#[test]
fn system_resolver_failures_are_not_cached_as_missing_addresses() {
    let mut harness = Harness::new(None);
    let first = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let (a, _) = system_requests(&mut harness);
    harness.system_resolved(a, Err("temporary failure".to_owned()));
    assert!(harness.result(first).is_err());

    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    assert_eq!(harness.system.len(), 1);
}

#[test]
fn system_resolver_answers_say_nothing_about_types_it_was_not_asked_about() {
    let mut harness = Harness::new(None);
    let first = harness.lookup_with(
        "example.com",
        &[ResourceType::A, ResourceType::MX],
        LookupOptions::default(),
    );
    let (a, aaaa) = system_requests(&mut harness);
    assert_eq!(aaaa, None);
    harness.system_resolved(a, Ok(vec![IpAddr::V4(A_ADDRESS)]));
    assert_eq!(harness.addresses(first), vec![IpAddr::V4(A_ADDRESS)]);

    let mx = harness.lookup_with("example.com", &[ResourceType::MX], LookupOptions::default());
    assert_eq!(harness.error(mx), "The system resolver can only look up addresses");
}

#[test]
fn system_resolver_finding_no_addresses_is_the_same_answer_before_and_after_caching() {
    let mut harness = Harness::new(None);
    let first = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let (a, _) = system_requests(&mut harness);
    harness.system_resolved(a, Ok(vec![]));
    assert!(harness.addresses(first).is_empty());

    let cached = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    assert!(harness.system.is_empty());
    assert!(harness.addresses(cached).is_empty());
}

#[test]
fn system_resolver_answers_from_before_a_reset_stay_out_of_the_cache() {
    let mut harness = Harness::new(None);
    let before = harness.lookup("example.com");
    let (a, aaaa) = system_requests(&mut harness);

    harness.resolver.set_server(
        Some(ServerConfig {
            host: "127.0.0.1".to_owned(),
            port: 53,
            transport: ServerTransport::Udp,
        }),
        harness.now,
    );
    harness.drain();
    harness.system_resolved(a, Ok(vec![IpAddr::V4(A_ADDRESS)]));
    harness.system_resolved(aaaa.unwrap(), Ok(vec![]));
    assert_eq!(harness.addresses(before), vec![IpAddr::V4(A_ADDRESS)]);

    // The configured server answers from now on.
    let after = harness.lookup("example.com");
    assert!(!harness.done.contains_key(&after));
    assert_eq!(harness.connects.len(), 1);
}

#[test]
fn record_lifetimes_start_when_their_answer_arrives() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup("example.com");
    harness.connected(harness.connects[0].connection);
    let writes = harness.take_writes();
    let ttl_10 = |response: &mut Message| {
        for answer in &mut response.answers {
            answer.ttl = 10;
        }
    };

    let (connection, a_query) = &writes[0];
    harness.receive(*connection, &respond(a_query, ttl_10));
    harness.now += Duration::from_secs(8);
    let (connection, aaaa_query) = &writes[1];
    harness.receive(*connection, &respond(aaaa_query, ttl_10));
    assert_eq!(harness.addresses(lookup).len(), 2);

    // The A record has 2 seconds left, not 10.
    harness.now += Duration::from_secs(2);
    harness.lookup("example.com");
    assert_eq!(harness.take_writes().len(), 2);
}

#[test]
fn zero_ttl_answers_reach_the_lookup_that_fetched_them_alongside_later_ones() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup("example.com");
    harness.connected(harness.connects[0].connection);
    let writes = harness.take_writes();
    let zero_ttl = |response: &mut Message| {
        for answer in &mut response.answers {
            answer.ttl = 0;
        }
    };
    let (connection, a_query) = &writes[0];
    harness.receive(*connection, &respond(a_query, zero_ttl));
    harness.now += Duration::from_millis(5);
    let (connection, aaaa_query) = &writes[1];
    harness.receive(*connection, &respond(aaaa_query, |_| {}));
    assert_eq!(harness.addresses(lookup).len(), 2);

    // The cache only has the AAAA record to offer.
    harness.lookup("example.com");
    assert_eq!(harness.take_writes().len(), 2);
}

#[test]
fn validated_record_lifetimes_start_when_the_answer_arrived() {
    let mut server = crate::dnssec::tests::make_server(false);
    server.start();
    let mut harness = dnssec_harness(&server);
    let lookup = harness.lookup_with("www.example", &[ResourceType::A], VALIDATE);
    harness.connected(harness.connects[0].connection);

    // The answer comes in at once, and what validating it takes comes in much later.
    let (connection, query) = harness.take_writes().remove(0);
    let query = Message::from_wire(&query).unwrap();
    let mut response = server.answer(&query.questions[0]);
    response.header.id = query.header.id;
    harness.receive(connection, &response.to_wire().unwrap());
    harness.now += Duration::from_secs(250);
    serve_signed_zones(&mut harness, &mut server);
    assert!(harness.result(lookup).is_ok());

    // The A record's 300 seconds count from when it arrived.
    harness.now += Duration::from_secs(51);
    harness.lookup_with("www.example", &[ResourceType::A], VALIDATE);
    assert!(!harness.writes.is_empty());
}

#[test]
fn query_ids_are_found_when_random_ones_keep_colliding() {
    let mut resolver = Resolver::new(Box::new(|| true), Box::new(|| Some(7)));
    let now = Instant::now();
    resolver.set_server(
        Some(ServerConfig {
            host: "127.0.0.1".to_owned(),
            port: 53,
            transport: ServerTransport::Udp,
        }),
        now,
    );
    resolver.lookup(
        "a.example",
        Class::IN,
        &[ResourceType::A],
        LookupOptions::default(),
        now,
    );
    resolver.lookup(
        "b.example",
        Class::IN,
        &[ResourceType::A],
        LookupOptions::default(),
        now,
    );
    let mut ids = resolver.in_flight_by_id.keys().copied().collect::<Vec<_>>();
    ids.sort_unstable();
    assert_eq!(ids, vec![7, 8]);
}

#[test]
fn server_is_reached_at_its_other_addresses_when_one_fails() {
    let mut harness = Harness::tls("dns.quad9.net");
    let first = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let requests = std::mem::take(&mut harness.system);
    // The IPv6 side answers first, so its address comes first.
    for (request, _, family) in requests.iter().rev() {
        let address = match family {
            AddressFamily::V4 => IpAddr::V4(Ipv4Addr::new(9, 9, 9, 9)),
            AddressFamily::V6 => IpAddr::V6("2620:fe::fe".parse().unwrap()),
        };
        harness.system_resolved(*request, Ok(vec![address]));
    }
    assert_eq!(harness.connects[0].address, "[2620:fe::fe]:853".parse().unwrap());
    harness
        .resolver
        .on_connect_failed(harness.connects[0].connection, "unreachable".to_owned(), harness.now);
    harness.drain();
    assert!(harness.result(first).is_err());

    // The next attempt goes to the IPv4 address, still verifying the certificate for the server's name.
    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    assert!(harness.system.is_empty());
    assert_eq!(harness.connects.len(), 2);
    assert_eq!(harness.connects[1].address, "9.9.9.9:853".parse().unwrap());
    assert_eq!(harness.connects[1].server_name, "dns.quad9.net");
}

fn server_v6() -> IpAddr {
    IpAddr::V6("2620:fe::fe".parse().unwrap())
}

fn server_v4() -> IpAddr {
    IpAddr::V4(Ipv4Addr::new(9, 9, 9, 9))
}

#[test]
fn server_addresses_that_come_in_after_connecting_started_are_tried_too() {
    let mut harness = Harness::tls("dns.quad9.net");
    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let (a, aaaa) = system_requests(&mut harness);
    harness.system_resolved(aaaa.unwrap(), Ok(vec![server_v6()]));
    harness.advance(IPV4_RESOLUTION_DELAY);
    assert_eq!(harness.connects.len(), 1);
    assert_eq!(harness.connects[0].address.ip(), server_v6());

    harness.system_resolved(a, Ok(vec![server_v4()]));
    harness
        .resolver
        .on_connect_failed(harness.connects[0].connection, "unreachable".to_owned(), harness.now);
    harness.drain();

    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    assert!(harness.system.is_empty());
    assert_eq!(harness.connects[1].address.ip(), server_v4());
}

#[test]
fn server_name_is_looked_up_again_once_all_its_addresses_failed() {
    let mut harness = Harness::tls("dns.quad9.net");
    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    for (request, _, family) in std::mem::take(&mut harness.system) {
        let address = if family == AddressFamily::V4 {
            server_v4()
        } else {
            server_v6()
        };
        harness.system_resolved(request, Ok(vec![address]));
    }
    for attempt in 0..2 {
        if attempt > 0 {
            harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
        }
        let connection = harness.connects.last().unwrap().connection;
        harness
            .resolver
            .on_connect_failed(connection, "unreachable".to_owned(), harness.now);
        harness.drain();
    }
    assert!(harness.system.is_empty());

    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    assert_eq!(harness.system.len(), 2);
}

#[test]
fn server_name_lookups_from_before_a_server_change_do_not_serve_the_new_server() {
    let mut harness = Harness::tls("dns.quad9.net");
    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let (a, aaaa) = system_requests(&mut harness);
    harness.system_resolved(aaaa.unwrap(), Ok(vec![server_v6()]));
    harness.advance(IPV4_RESOLUTION_DELAY);
    assert_eq!(harness.connects.len(), 1);

    // Only the port changes, while the IPv4 side of the server's name is still being looked up.
    let server = ServerConfig {
        host: "dns.quad9.net".to_owned(),
        port: 8853,
        transport: ServerTransport::Tls,
    };
    harness.resolver.set_server(Some(server), harness.now);
    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let (new_a, new_aaaa) = system_requests(&mut harness);
    harness.system_resolved(a, Ok(vec![server_v4()]));

    // The new server gets the addresses of its own lookup.
    harness.system_resolved(new_aaaa.unwrap(), Ok(vec![server_v6()]));
    harness.system_resolved(new_a, Ok(vec![server_v4()]));
    let connect = harness.connects.last().unwrap();
    assert_eq!(connect.address.port(), 8853);
    let connection = connect.connection;
    harness
        .resolver
        .on_connect_failed(connection, "unreachable".to_owned(), harness.now);
    harness.drain();
    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let addresses: Vec<IpAddr> = harness
        .connects
        .iter()
        .skip(1)
        .map(|connect| connect.address.ip())
        .collect();
    assert!(addresses.contains(&server_v4()) && addresses.contains(&server_v6()));
}

#[test]
fn late_udp_answers_do_not_count_once_a_question_moved_to_tcp() {
    let mut harness = Harness::udp();
    let lookup = harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let udp = harness.connects[0].connection;
    harness.connected(udp);
    let (_, query) = harness.take_writes().remove(0);
    let truncated = respond(&query, |response| {
        response.header.options.set_is_truncated(true);
        response.answers.clear();
    });
    harness.receive(udp, &truncated);
    let tcp = harness.connects[1].connection;
    harness.connected(tcp);
    let (_, tcp_query) = harness.take_writes().remove(0);

    // A full answer over UDP that shows up late is not the one asked for now.
    let late = respond(&query, |response| response.answers.truncate(1));
    harness.receive(udp, &late);
    assert!(!harness.done.contains_key(&lookup));

    harness.receive(tcp, &frame(&respond(unframe(&tcp_query), |_| {})));
    assert!(harness.result(lookup).is_ok());
}

#[test]
fn system_resolver_family_that_never_answers_is_given_up_on() {
    let mut harness = Harness::new(None);
    let lookup = harness.lookup("example.com");
    let (a, aaaa) = system_requests(&mut harness);
    // The A side's answer gets lost. The AAAA side finds nothing, which starts no grace window.
    harness.system_resolved(aaaa.unwrap(), Ok(vec![]));
    assert_eq!(
        harness.resolver.next_deadline(),
        Some(harness.now + SYSTEM_RESOLVER_TIMEOUT)
    );

    harness.advance(SYSTEM_RESOLVER_TIMEOUT);
    assert_eq!(harness.error(lookup), "Could not resolve to IPv4 or IPv6 address");
    assert_eq!(harness.resolver.next_deadline(), None);

    assert!(harness.resolver.system_requests.is_empty());
    assert!(harness.cancelled.contains(&a));

    // That the A side never answered says nothing about whether there are IPv4 addresses.
    harness.lookup("example.com");
    assert_eq!(harness.system.len(), 2);
}

#[test]
fn late_system_resolver_answers_do_not_replace_newer_cached_addresses() {
    let mut harness = Harness::new(None);
    let both = harness.lookup("example.com");
    let (a, stalled_aaaa) = system_requests(&mut harness);
    harness.system_resolved(a, Ok(vec![IpAddr::V4(Ipv4Addr::new(192, 0, 2, 1))]));
    harness.advance(IPV6_RESOLUTION_DELAY);
    assert!(harness.result(both).is_ok());

    // A later IPv4-only lookup finds a newer address.
    harness.now += Duration::from_secs(10);
    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let (a, _) = system_requests(&mut harness);
    harness.system_resolved(a, Ok(vec![IpAddr::V4(Ipv4Addr::new(192, 0, 2, 2))]));

    // The first lookup's IPv6 side finishes now: its addresses join the cache, its older IPv4 one does not.
    harness.system_resolved(stalled_aaaa.unwrap(), Ok(vec![IpAddr::V6(Ipv6Addr::LOCALHOST)]));
    let cached = harness.lookup("example.com");
    assert!(harness.system.is_empty());
    let mut addresses = harness.addresses(cached);
    addresses.sort();
    assert_eq!(
        addresses,
        vec![IpAddr::V4(Ipv4Addr::new(192, 0, 2, 2)), IpAddr::V6(Ipv6Addr::LOCALHOST)]
    );
}

#[test]
fn resolutions_finishing_within_their_grace_window_keep_newer_cached_addresses() {
    let mut harness = Harness::new(None);
    harness.lookup("example.com");
    let (a, aaaa) = system_requests(&mut harness);
    harness.system_resolved(a, Ok(vec![IpAddr::V4(Ipv4Addr::new(192, 0, 2, 1))]));

    harness.now += Duration::from_millis(20);
    harness.lookup_with("example.com", &[ResourceType::A], LookupOptions::default());
    let (newer_a, _) = system_requests(&mut harness);
    harness.system_resolved(newer_a, Ok(vec![IpAddr::V4(Ipv4Addr::new(192, 0, 2, 2))]));

    // The first resolution's IPv6 side finishes before its grace window ends.
    harness.now += Duration::from_millis(20);
    harness.system_resolved(aaaa.unwrap(), Ok(vec![IpAddr::V6(Ipv6Addr::LOCALHOST)]));
    let cached = harness.lookup("example.com");
    assert!(harness.system.is_empty());
    let mut addresses = harness.addresses(cached);
    addresses.sort();
    assert_eq!(
        addresses,
        vec![IpAddr::V4(Ipv4Addr::new(192, 0, 2, 2)), IpAddr::V6(Ipv6Addr::LOCALHOST)]
    );
}

#[test]
fn validated_negative_answers_last_no_longer_than_their_proof() {
    let mut server = crate::dnssec::tests::make_server(false);
    server.expire_example_signatures_in(2);
    server.start();
    let mut harness = dnssec_harness(&server);
    let lookup = harness.lookup_with("www.example", &[ResourceType::AAAA], VALIDATE);
    harness.connected(harness.connects[0].connection);
    serve_signed_zones(&mut harness, &mut server);
    let result = harness.result(lookup).as_ref().unwrap();
    assert_eq!(result.security_status, Some(SecurityStatus::Secure));
    assert!(result.records.is_empty());

    harness.lookup_with("www.example", &[ResourceType::AAAA], VALIDATE);
    assert!(harness.writes.is_empty());

    // The SOA allows longer, but the proof's signatures run out.
    harness.now += Duration::from_secs(2);
    harness.lookup_with("www.example", &[ResourceType::AAAA], VALIDATE);
    assert!(!harness.writes.is_empty());
}
