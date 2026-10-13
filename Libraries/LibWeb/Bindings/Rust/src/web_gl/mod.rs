/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebContent half of WebGL, which validates what script asks for, keeps track of the state that it can read
//! back, and records everything else as commands for the Compositor, which runs GL.

pub(crate) mod constants;
pub(crate) mod proxy;
