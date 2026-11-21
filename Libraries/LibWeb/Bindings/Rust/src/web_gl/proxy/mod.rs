/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The recording side of the WebGL command stream. Libraries/LibCompositing/WebGL is the replaying side, and
//! GLFunctions.json there defines what crosses the stream for both, as libcompositing_rust::webgl.

pub(crate) mod custom;
pub(crate) mod generated;
pub(crate) mod recorder;
pub(crate) mod transport;

#[cfg(test)]
mod tests;
