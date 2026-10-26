/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Manual signaling helpers for stdio-based interop with pion/aiortc demos.
//!
//! Wire format: a single line of base64-encoded JSON `{ "type": ..., "sdp": ... }`,
//! the same shape pion's `examples/signal/signal.go` uses. Trickle ICE is off;
//! candidates ride inside the SDP after `gathering_complete_promise()` resolves.

use std::io::{self, BufRead, Write};

use anyhow::{Context, Result};
use base64::Engine;
use base64::engine::general_purpose::STANDARD as B64;
use webrtc::peer_connection::sdp::session_description::RTCSessionDescription;

pub fn encode(desc: &RTCSessionDescription) -> Result<String> {
    let json = serde_json::to_string(desc)?;
    Ok(B64.encode(json.as_bytes()))
}

pub fn decode(line: &str) -> Result<RTCSessionDescription> {
    let bytes = B64.decode(line.trim()).context("base64 decode")?;
    let desc = serde_json::from_slice::<RTCSessionDescription>(&bytes).context("json decode")?;
    Ok(desc)
}

/// Read a single line from stdin and decode it as an `RTCSessionDescription`.
pub fn read_from_stdin() -> Result<RTCSessionDescription> {
    let mut line = String::new();
    let stdin = io::stdin();
    stdin.lock().read_line(&mut line).context("read sdp from stdin")?;
    if line.trim().is_empty() {
        anyhow::bail!("empty SDP on stdin");
    }
    decode(&line)
}

pub fn write_to_stdout(desc: &RTCSessionDescription) -> Result<()> {
    let encoded = encode(desc)?;
    let stdout = io::stdout();
    let mut out = stdout.lock();
    writeln!(out, "{encoded}")?;
    out.flush()?;
    Ok(())
}
