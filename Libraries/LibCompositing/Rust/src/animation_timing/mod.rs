/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The timing model of Web Animations: how far along an animation effect is, from its timing
//! properties and the state of its animation and timeline.
//! https://www.w3.org/TR/web-animations-1/#timing-model

pub mod calculations;
pub mod effect_timing;
pub mod playback;
pub mod time_value;

pub use calculations::*;
pub use effect_timing::*;
pub use playback::*;
pub use time_value::{OptionalNumber, OptionalTimeValue, TimeValue, TimeValueType};

#[cfg(test)]
mod tests;
