/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Event Timing, https://w3c.github.io/event-timing/

pub(crate) mod event_counts;
#[allow(
    clippy::module_inception,
    reason = "the module mirrors EventTiming/EventTiming.cpp, which the crate module is named after"
)]
pub(crate) mod event_timing;
pub(crate) mod performance_event_timing;
