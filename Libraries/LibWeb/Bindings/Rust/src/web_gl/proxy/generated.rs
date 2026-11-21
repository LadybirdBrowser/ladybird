/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The methods of the recorder that follow from the signature of a GL function. The build script writes them from
//! GLFunctions.json (generate/webgl_proxy.rs).

#![allow(
    dead_code,
    reason = "the recorder has a method for every GL function of the replayer, of which the contexts call some"
)]

use super::recorder::{
    Proxy, bytes_of, bytes_of_mut, copy_reply_data, gen_count, leading_bytes, leading_bytes_mut, payload_size,
    until_nul,
};
use libcompositing_rust::webgl::vocabulary::{commands, sync_calls};
use libcompositing_rust::webgl::wire::{DataSpan, InlineData, WireStruct};

use super::transport::Transport;
use crate::web_gl::constants::GL_COLOR;

include!(concat!(env!("OUT_DIR"), "/webgl_proxy.rs"));
