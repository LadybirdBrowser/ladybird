/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The GL functions that the replayer calls, which the build script lists from GLFunctions.json
//! (generate/webgl_replayer.rs). They are looked up rather than linked, so that the crate links without ANGLE.

#![allow(clippy::missing_safety_doc)]

include!(concat!(env!("OUT_DIR"), "/webgl_gl.rs"));
