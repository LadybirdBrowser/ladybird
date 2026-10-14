/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.14

use super::state::{RenderingContext, WebGL1};

pub(crate) type WebGLRenderingContext = RenderingContext<WebGL1>;

/// https://registry.khronos.org/webgl/specs/latest/1.0/#5.2
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum WebGLPowerPreference {
    Default,
    LowPower,
    HighPerformance,
}
