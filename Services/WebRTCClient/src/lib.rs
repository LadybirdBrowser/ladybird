/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebRTCClient service, which owns the entire WebRTC stack via `ladybird_webrtc`.
//!
//! The C++ entry point in main.cpp sandboxes the process and then calls `webrtc_client_run`. The Browser hands the
//! other end of the transport to the WebContent process it serves: on Apple platforms through the Browser's Mach
//! bootstrap server, elsewhere as an inherited socket or Windows pipe named by `SOCKET_TAKEOVER=ipc:<fd>`.

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../Libraries/RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../Libraries/RustPanic.rs"]
mod rust_panic;

use anyhow::Result;
use ladybird_webrtc::ipc::Transport;
use ladybird_webrtc::service::{self, NetworkAccess};
use std::time::Duration;

#[cfg(all(unix, not(target_vendor = "apple")))]
fn take_over_transport(_: &str) -> Result<Transport> {
    use anyhow::{Context, anyhow};
    use std::os::fd::{FromRawFd, RawFd};
    use std::os::unix::net::UnixStream;

    let takeover = std::env::var("SOCKET_TAKEOVER").context("SOCKET_TAKEOVER is not set")?;
    let fd = takeover
        .split(';')
        .find_map(|entry| entry.strip_prefix("ipc:"))
        .ok_or_else(|| anyhow!("SOCKET_TAKEOVER has no ipc entry: '{takeover}'"))?;
    let fd: RawFd = fd
        .parse()
        .with_context(|| format!("invalid fd in SOCKET_TAKEOVER: '{takeover}'"))?;
    // NB: Our children must not think that they were handed this socket too.
    unsafe { std::env::remove_var("SOCKET_TAKEOVER") };
    // NB: The Browser gave this process its end of a connected socket, which nothing else here owns.
    let socket = unsafe { UnixStream::from_raw_fd(fd) };
    Ok(Transport::new(socket)?)
}

#[cfg(windows)]
fn take_over_transport(_: &str) -> Result<Transport> {
    Ok(Transport::take_over()?)
}

#[cfg(target_vendor = "apple")]
fn take_over_transport(server_name: &str) -> Result<Transport> {
    use anyhow::{Context, anyhow};
    use libipc_rust::mach;

    if server_name.is_empty() {
        return Err(anyhow!("no --mach-server-name was given"));
    }
    let server_port = mach::look_up_from_bootstrap_server(server_name)
        .with_context(|| format!("looking up Mach server '{server_name}'"))?;
    let (receive_right, send_right) =
        mach::bootstrap_transport_from_server_port(&server_port).context("bootstrapping the transport")?;
    Ok(Transport::new(receive_right, send_right)?)
}

fn run(mach_server_name: &str, network_access: NetworkAccess) -> Result<()> {
    // webrtc-rs warns about plenty of ordinary connection setup and teardown, so only its errors are shown by default.
    env_logger::Builder::from_env(
        env_logger::Env::default().default_filter_or("error,ladybird_webrtc=warn,webrtcclient_service=warn"),
    )
    .target(env_logger::Target::Stderr)
    .init();

    let transport = take_over_transport(mach_server_name)?;
    let runtime = tokio::runtime::Builder::new_multi_thread().enable_all().build()?;
    log::info!("WebRTCClient running");
    let result = runtime.block_on(service::run(transport, network_access));
    // Dropping the runtime would wait for every blocking task, however long one takes.
    runtime.shutdown_timeout(Duration::from_secs(1));
    result
}

/// Runs the service until its client goes away, returning the process exit code.
///
/// # Safety
/// `mach_server_name` must point to `mach_server_name_length` readable bytes, or be null with a length of zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webrtc_client_run(
    mach_server_name: *const u8,
    mach_server_name_length: usize,
    network_access_allowed: bool,
) -> i32 {
    // SAFETY: The caller promises that the bytes are readable. They come from the command line, so they are UTF-8 in
    //         practice; anything else is no server name.
    let bytes = unsafe { ak::slice_from_raw_parts(mach_server_name, mach_server_name_length) };
    let mach_server_name = std::str::from_utf8(bytes).unwrap_or("");

    let network_access = if network_access_allowed {
        NetworkAccess::Allowed
    } else {
        NetworkAccess::Denied
    };
    match run(mach_server_name, network_access) {
        Ok(()) => 0,
        Err(error) => {
            eprintln!("WebRTCClient: {error:#}");
            1
        }
    }
}
