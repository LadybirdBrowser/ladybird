/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Gamepad API, https://w3c.github.io/gamepad/

pub(crate) mod exports;
#[allow(clippy::module_inception, reason = "the module mirrors the Gamepad interface file")]
pub(crate) mod gamepad;
pub(crate) mod gamepad_button;
pub(crate) mod gamepad_event;
pub(crate) mod gamepad_haptic_actuator;
pub(crate) mod navigator_gamepad;
