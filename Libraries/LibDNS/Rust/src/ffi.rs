/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI that LibDNS's C++ Resolver drives the resolver through.

use crate::message::{Class, ResourceType};
use crate::resolver::{
    Action, AddressFamily, ConnectKind, LookupOptions, LookupResult, Resolver, SecurityStatus, ServerConfig,
    ServerTransport,
};
use crate::rust_panic::abort_on_panic;
use std::ffi::c_void;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};
use std::sync::Arc;
use std::time::{Duration, Instant};

/// An IPv4 address is in the first four octets.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FfiAddress {
    pub is_ipv6: bool,
    pub octets: [u8; 16],
    pub port: u16,
}

impl FfiAddress {
    fn from_ip(address: IpAddr, port: u16) -> Self {
        let mut octets = [0; 16];
        match address {
            IpAddr::V4(address) => octets[..4].copy_from_slice(&address.octets()),
            IpAddr::V6(address) => octets = address.octets(),
        }
        Self {
            is_ipv6: address.is_ipv6(),
            octets,
            port,
        }
    }

    fn to_ip(self) -> IpAddr {
        if self.is_ipv6 {
            IpAddr::V6(Ipv6Addr::from(self.octets))
        } else {
            IpAddr::V4(Ipv4Addr::new(
                self.octets[0],
                self.octets[1],
                self.octets[2],
                self.octets[3],
            ))
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiBytes {
    pub data: *const u8,
    pub length: usize,
}

impl FfiBytes {
    const EMPTY: Self = Self {
        data: std::ptr::null(),
        length: 0,
    };

    fn of(bytes: &[u8]) -> Self {
        Self {
            data: bytes.as_ptr(),
            length: bytes.len(),
        }
    }

    unsafe fn as_slice<'a>(self) -> &'a [u8] {
        if self.length == 0 {
            return &[];
        }
        unsafe { ak::slice_from_raw_parts(self.data, self.length) }
    }

    unsafe fn to_string_lossy(self) -> String {
        String::from_utf8_lossy(unsafe { self.as_slice() }).into_owned()
    }
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiServerTransport {
    None,
    Udp,
    Tls,
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiConnectKind {
    Udp,
    Tcp,
    Tls,
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiActionKind {
    Connect,
    Write,
    Close,
    SystemResolve,
    CancelSystemResolve,
    LookupSucceeded,
    LookupFailed,
}

/// What `dns_resolver_take_action()` hands out. Only the fields its kind names are set. Everything it points to stays
/// valid until the next call on the resolver.
#[repr(C)]
pub struct FfiAction {
    pub kind: FfiActionKind,
    /// Connect, Write, Close.
    pub connection: u64,
    /// Connect.
    pub connect_kind: FfiConnectKind,
    pub address: FfiAddress,
    pub server_name: FfiBytes,
    /// Write.
    pub bytes: FfiBytes,
    /// SystemResolve, CancelSystemResolve.
    pub request: u64,
    pub name: FfiBytes,
    pub ipv6: bool,
    /// LookupSucceeded, LookupFailed.
    pub lookup: u64,
    /// LookupSucceeded: release it with `dns_lookup_result_release()`.
    pub result: *const LookupResult,
    /// LookupFailed: a message that stays valid for as long as the process runs, and maybe some detail.
    pub error: FfiBytes,
    pub error_detail: FfiBytes,
}

impl Default for FfiAction {
    fn default() -> Self {
        Self {
            kind: FfiActionKind::Close,
            connection: 0,
            connect_kind: FfiConnectKind::Udp,
            address: FfiAddress::default(),
            server_name: FfiBytes::EMPTY,
            bytes: FfiBytes::EMPTY,
            request: 0,
            name: FfiBytes::EMPTY,
            ipv6: false,
            lookup: 0,
            result: std::ptr::null(),
            error: FfiBytes::EMPTY,
            error_detail: FfiBytes::EMPTY,
        }
    }
}

pub type FfiIsIpv6GloballyReachable = unsafe extern "C" fn(context: *mut c_void) -> bool;

pub struct DnsResolver {
    resolver: Resolver,
    epoch: Instant,
    epoch_ms: u64,
    // What the last action handed out points into.
    current: Option<Action>,
}

impl DnsResolver {
    fn instant(&self, now_ms: u64) -> Instant {
        self.epoch + Duration::from_millis(now_ms.saturating_sub(self.epoch_ms))
    }
}

struct ReachabilityCallback {
    callback: FfiIsIpv6GloballyReachable,
    context: *mut c_void,
}

/// # Safety
/// `callback` is called with `context` from inside calls on the resolver, for as long as it lives.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_resolver_create(
    now_ms: u64,
    is_ipv6_globally_reachable: FfiIsIpv6GloballyReachable,
    context: *mut c_void,
) -> *mut DnsResolver {
    abort_on_panic(|| {
        let reachability = ReachabilityCallback {
            callback: is_ipv6_globally_reachable,
            context,
        };
        let resolver = Resolver::new(
            Box::new(move || unsafe { (reachability.callback)(reachability.context) }),
            Box::new(|| {
                let mut id = [0u8; 2];
                libcrypto_rust::fill_with_random(&mut id).ok()?;
                Some(u16::from_ne_bytes(id))
            }),
        );
        Box::into_raw(Box::new(DnsResolver {
            resolver,
            epoch: Instant::now(),
            epoch_ms: now_ms,
            current: None,
        }))
    })
}

/// # Safety
/// `resolver` came from `dns_resolver_create()` and is not used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_resolver_destroy(resolver: *mut DnsResolver) {
    abort_on_panic(|| {
        if !resolver.is_null() {
            drop(unsafe { Box::from_raw(resolver) });
        }
    });
}

/// # Safety
/// `resolver` came from `dns_resolver_create()`; `host` points to `host.length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_resolver_set_server(
    resolver: &mut DnsResolver,
    transport: FfiServerTransport,
    host: FfiBytes,
    port: u16,
    now_ms: u64,
) {
    abort_on_panic(|| {
        let transport = match transport {
            FfiServerTransport::None => None,
            FfiServerTransport::Udp => Some(ServerTransport::Udp),
            FfiServerTransport::Tls => Some(ServerTransport::Tls),
        };
        let server = transport.map(|transport| ServerConfig {
            host: unsafe { host.to_string_lossy() },
            port,
            transport,
        });
        let now = resolver.instant(now_ms);
        resolver.resolver.set_server(server, now);
    });
}

/// # Safety
/// `domains` points to `count` byte strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_resolver_set_loopback_domains(
    resolver: &mut DnsResolver,
    domains: *const FfiBytes,
    count: usize,
) {
    abort_on_panic(|| {
        let domains = if count == 0 {
            &[][..]
        } else {
            unsafe { ak::slice_from_raw_parts(domains, count) }
        };
        let domains = domains
            .iter()
            .map(|domain| unsafe { domain.to_string_lossy() })
            .collect();
        resolver.resolver.set_loopback_domains(domains);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn dns_resolver_set_system_resolver_timeout(resolver: &mut DnsResolver, timeout_ms: u64) {
    abort_on_panic(|| {
        resolver
            .resolver
            .set_system_resolver_timeout(Duration::from_millis(timeout_ms));
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn dns_resolver_reset_connection(resolver: &mut DnsResolver, now_ms: u64) {
    abort_on_panic(|| {
        let now = resolver.instant(now_ms);
        resolver.resolver.reset_connection(now);
    });
}

/// # Safety
/// `name` points to `name.length` bytes, and `types` to `type_count` resource types.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_resolver_lookup(
    resolver: &mut DnsResolver,
    name: FfiBytes,
    class: u16,
    types: *const u16,
    type_count: usize,
    validate_dnssec_locally: bool,
    query_aaaa_without_global_ipv6: bool,
    now_ms: u64,
) -> u64 {
    abort_on_panic(|| {
        let name = unsafe { name.to_string_lossy() };
        let types = if type_count == 0 {
            &[][..]
        } else {
            unsafe { ak::slice_from_raw_parts(types, type_count) }
        };
        let types: Vec<ResourceType> = types.iter().map(|rtype| ResourceType(*rtype)).collect();
        let options = LookupOptions {
            validate_dnssec_locally,
            query_aaaa_without_global_ipv6,
        };
        let now = resolver.instant(now_ms);
        resolver.resolver.lookup(&name, Class(class), &types, options, now)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dns_resolver_on_connected(resolver: &mut DnsResolver, connection: u64, now_ms: u64) {
    abort_on_panic(|| {
        let now = resolver.instant(now_ms);
        resolver.resolver.on_connected(connection, now);
    });
}

/// # Safety
/// `error` points to `error.length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_resolver_on_connect_failed(
    resolver: &mut DnsResolver,
    connection: u64,
    error: FfiBytes,
    now_ms: u64,
) {
    abort_on_panic(|| {
        let error = unsafe { error.to_string_lossy() };
        let now = resolver.instant(now_ms);
        resolver.resolver.on_connect_failed(connection, error, now);
    });
}

/// # Safety
/// `bytes` points to `bytes.length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_resolver_on_received(
    resolver: &mut DnsResolver,
    connection: u64,
    bytes: FfiBytes,
    now_ms: u64,
) {
    abort_on_panic(|| {
        let now = resolver.instant(now_ms);
        resolver
            .resolver
            .on_received(connection, unsafe { bytes.as_slice() }, now);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn dns_resolver_on_eof(resolver: &mut DnsResolver, connection: u64, now_ms: u64) {
    abort_on_panic(|| {
        let now = resolver.instant(now_ms);
        resolver.resolver.on_eof(connection, now);
    });
}

/// # Safety
/// When `succeeded`, `addresses` points to `count` addresses.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_resolver_on_system_resolved(
    resolver: &mut DnsResolver,
    request: u64,
    succeeded: bool,
    addresses: *const FfiAddress,
    count: usize,
    now_ms: u64,
) {
    abort_on_panic(|| {
        let result = if succeeded {
            let addresses = if count == 0 {
                &[][..]
            } else {
                unsafe { ak::slice_from_raw_parts(addresses, count) }
            };
            Ok(addresses.iter().map(|address| address.to_ip()).collect())
        } else {
            Err("The system resolver failed".to_owned())
        };
        let now = resolver.instant(now_ms);
        resolver.resolver.on_system_resolved(request, result, now);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn dns_resolver_on_tick(resolver: &mut DnsResolver, now_ms: u64) {
    abort_on_panic(|| {
        let now = resolver.instant(now_ms);
        resolver.resolver.on_tick(now);
    });
}

/// Milliseconds until `dns_resolver_on_tick()` next has something to do, or -1 when it has nothing to do.
#[unsafe(no_mangle)]
pub extern "C" fn dns_resolver_next_deadline(resolver: &DnsResolver, now_ms: u64) -> i64 {
    abort_on_panic(|| {
        let Some(deadline) = resolver.resolver.next_deadline() else {
            return -1;
        };
        let wait = deadline.saturating_duration_since(resolver.instant(now_ms));
        // NB: Round up, so that the tick lands on or after the deadline.
        let wait = wait.as_millis() + u128::from(wait.subsec_nanos() % 1_000_000 != 0);
        i64::try_from(wait).unwrap_or(i64::MAX)
    })
}

/// Fills `action` with the next thing to do, and returns whether there was one.
///
/// # Safety
/// `action` points to an `FfiAction`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_resolver_take_action(resolver: &mut DnsResolver, action: &mut FfiAction) -> bool {
    abort_on_panic(|| {
        resolver.current = resolver.resolver.take_action();
        let Some(current) = &resolver.current else { return false };
        *action = FfiAction::default();
        match current {
            Action::Connect {
                connection,
                kind,
                address,
                server_name,
            } => {
                action.kind = FfiActionKind::Connect;
                action.connection = *connection;
                action.connect_kind = match kind {
                    ConnectKind::Udp => FfiConnectKind::Udp,
                    ConnectKind::Tcp => FfiConnectKind::Tcp,
                    ConnectKind::Tls => FfiConnectKind::Tls,
                };
                action.address = socket_address(*address);
                action.server_name = FfiBytes::of(server_name.as_bytes());
            }
            Action::Write { connection, bytes } => {
                action.kind = FfiActionKind::Write;
                action.connection = *connection;
                action.bytes = FfiBytes::of(bytes);
            }
            Action::Close { connection } => {
                action.kind = FfiActionKind::Close;
                action.connection = *connection;
            }
            Action::SystemResolve { request, name, family } => {
                action.kind = FfiActionKind::SystemResolve;
                action.request = *request;
                action.name = FfiBytes::of(name.as_bytes());
                action.ipv6 = *family == AddressFamily::V6;
            }
            Action::CancelSystemResolve { request } => {
                action.kind = FfiActionKind::CancelSystemResolve;
                action.request = *request;
            }
            Action::LookupDone { lookup, result } => {
                action.lookup = *lookup;
                match result {
                    Ok(result) => {
                        action.kind = FfiActionKind::LookupSucceeded;
                        action.result = Arc::into_raw(result.clone());
                    }
                    Err(error) => {
                        action.kind = FfiActionKind::LookupFailed;
                        action.error = FfiBytes::of(error.message.as_bytes());
                        if let Some(detail) = &error.detail {
                            action.error_detail = FfiBytes::of(detail.as_bytes());
                        }
                    }
                }
            }
        }
        true
    })
}

fn socket_address(address: SocketAddr) -> FfiAddress {
    FfiAddress::from_ip(address.ip(), address.port())
}

// ---- Lookup results ----------------------------------------------------------------------------------------------

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiSecurityStatus {
    NotValidated,
    Secure,
    Insecure,
}

/// One record of a lookup result. Its text is the record's data, as `dns` prints it.
#[repr(C)]
pub struct FfiRecord {
    pub rtype: u16,
    pub ttl: u32,
    pub has_address: bool,
    pub address: FfiAddress,
}

/// # Safety
/// `result` came from a LookupSucceeded action and is not used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_lookup_result_release(result: *const LookupResult) {
    abort_on_panic(|| {
        if !result.is_null() {
            drop(unsafe { Arc::from_raw(result) });
        }
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn dns_lookup_result_security_status(result: &LookupResult) -> FfiSecurityStatus {
    match result.security_status {
        None => FfiSecurityStatus::NotValidated,
        Some(SecurityStatus::Secure) => FfiSecurityStatus::Secure,
        Some(SecurityStatus::Insecure) => FfiSecurityStatus::Insecure,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn dns_lookup_result_record_count(result: &LookupResult) -> usize {
    result.records.len()
}

/// # Safety
/// `index` is below `dns_lookup_result_record_count()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_lookup_result_record(result: &LookupResult, index: usize) -> FfiRecord {
    abort_on_panic(|| {
        let record = &result.records[index];
        let address = match record.data {
            crate::message::RecordData::A(address) => Some(IpAddr::V4(address)),
            crate::message::RecordData::Aaaa(address) => Some(IpAddr::V6(address)),
            _ => None,
        };
        FfiRecord {
            rtype: record.rtype.0,
            ttl: record.ttl,
            has_address: address.is_some(),
            address: address
                .map(|address| FfiAddress::from_ip(address, 0))
                .unwrap_or_default(),
        }
    })
}

pub type FfiStringCallback = unsafe extern "C" fn(context: *mut c_void, string: FfiBytes);

/// Calls `callback` with the text of the record's data.
///
/// # Safety
/// `index` is below `dns_lookup_result_record_count()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_lookup_result_record_text(
    result: &LookupResult,
    index: usize,
    callback: FfiStringCallback,
    context: *mut c_void,
) {
    abort_on_panic(|| {
        let text = result.records[index].data.to_string();
        unsafe { callback(context, FfiBytes::of(text.as_bytes())) };
    });
}

/// Calls `callback` with the name the lookup was for.
///
/// # Safety
/// `callback` takes `context`, and what it is given is only valid during the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_lookup_result_name(
    result: &LookupResult,
    callback: FfiStringCallback,
    context: *mut c_void,
) {
    abort_on_panic(|| {
        let name = result.name.to_string();
        unsafe { callback(context, FfiBytes::of(name.as_bytes())) };
    });
}

// ---- Resource types ----------------------------------------------------------------------------------------------

/// Calls `callback` with the type's name, as `dns` prints it.
///
/// # Safety
/// `callback` takes `context`, and what it is given is only valid during the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_resource_type_name(rtype: u16, callback: FfiStringCallback, context: *mut c_void) {
    abort_on_panic(|| {
        let name = ResourceType(rtype).name();
        unsafe { callback(context, FfiBytes::of(name.as_bytes())) };
    });
}

/// Returns whether `name` names a resource type, and if so puts it in `rtype`.
///
/// # Safety
/// `name` points to `name.length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dns_resource_type_from_name(name: FfiBytes, rtype: &mut u16) -> bool {
    abort_on_panic(|| {
        let name = unsafe { name.to_string_lossy() };
        match ResourceType::from_name(&name) {
            Some(found) => {
                *rtype = found.0;
                true
            }
            None => false,
        }
    })
}
