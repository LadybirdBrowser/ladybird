/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Performance Timeline, https://w3c.github.io/performance-timeline/

pub(crate) mod entry_types;
pub(crate) mod exports;
pub(crate) mod performance_entry;
pub(crate) mod performance_entry_tuple;
pub(crate) mod performance_observer;
pub(crate) mod performance_observer_entry_list;
#[allow(
    clippy::module_inception,
    reason = "the module mirrors the specification's performance timeline, which the crate module is named after"
)]
pub(crate) mod performance_timeline;
