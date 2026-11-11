/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_rust::api::{DeferGc, Trace};

use crate::performance_timeline::entry_types::SupportedEntryType;
use crate::performance_timeline::performance_entry::CppPerformanceEntry;

// https://w3c.github.io/performance-timeline/#dfn-performance-entry-buffer-map
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceEntryTuple {
    /// The key of the tuple in the map.
    #[gc(untraced)]
    pub(crate) entry_type: Utf16String,

    // https://w3c.github.io/performance-timeline/#dfn-performance-entry-buffer
    // A performance entry buffer to store PerformanceEntry objects, that is initially empty.
    pub(crate) performance_entry_buffer: Vec<CppPerformanceEntry>,

    // https://w3c.github.io/performance-timeline/#dfn-maxbuffersize
    // An integer maxBufferSize, initialized to the registry value for this entry type.
    // NB: None is an infinite size.
    #[gc(untraced)]
    pub(crate) max_buffer_size: Option<u64>,

    // https://w3c.github.io/performance-timeline/#dfn-availablefromtimeline
    // A boolean availableFromTimeline, initialized to the registry value for this entry type.
    pub(crate) available_from_timeline: bool,

    // https://w3c.github.io/performance-timeline/#dfn-dropped-entries-count
    // An integer dropped entries count that is initially 0.
    pub(crate) dropped_entries_count: u64,
}

impl PerformanceEntryTuple {
    pub(crate) fn new(_: &DeferGc<'_>, supported_entry_type: &SupportedEntryType) -> Self {
        Self {
            entry_type: Utf16String::from_utf8(supported_entry_type.entry_type),
            performance_entry_buffer: Vec::new(),
            max_buffer_size: supported_entry_type.max_buffer_size,
            available_from_timeline: supported_entry_type.available_from_timeline,
            dropped_entries_count: 0,
        }
    }

    // https://w3c.github.io/performance-timeline/#dfn-determine-if-a-performance-entry-buffer-is-full
    pub(crate) fn is_full(&mut self) -> bool {
        // 1. Let num current entries be the size of tuple's performance entry buffer.
        let num_current_entries = self.performance_entry_buffer.len() as u64;

        // 2. If num current entries is less than tuples's maxBufferSize, return false.
        if self
            .max_buffer_size
            .is_none_or(|max_buffer_size| num_current_entries < max_buffer_size)
        {
            return false;
        }

        // 3. Increase tuple's dropped entries count by 1.
        self.dropped_entries_count += 1;

        // 4. Return true.
        true
    }
}
