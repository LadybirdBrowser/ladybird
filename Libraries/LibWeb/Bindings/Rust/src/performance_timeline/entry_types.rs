/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The timing entry types registry, https://w3c.github.io/timing-entrytypes-registry/#registry

pub(crate) struct SupportedEntryType {
    pub(crate) entry_type: &'static str,
    pub(crate) window_only: bool,
    /// https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
    pub(crate) available_from_timeline: bool,
    /// https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
    // NB: None is an infinite size.
    pub(crate) max_buffer_size: Option<u64>,
}

// NB: In alphabetical order of their entry type, which supportedEntryTypes lists them in.
pub(crate) const SUPPORTED_ENTRY_TYPES: &[SupportedEntryType] = &[
    SupportedEntryType {
        entry_type: "event",
        window_only: true,
        available_from_timeline: false,
        max_buffer_size: Some(150),
    },
    SupportedEntryType {
        entry_type: "first-input",
        window_only: true,
        available_from_timeline: true,
        max_buffer_size: Some(1),
    },
    SupportedEntryType {
        entry_type: "largest-contentful-paint",
        window_only: true,
        available_from_timeline: false,
        max_buffer_size: Some(150),
    },
    SupportedEntryType {
        entry_type: "mark",
        window_only: false,
        available_from_timeline: true,
        max_buffer_size: None,
    },
    SupportedEntryType {
        entry_type: "measure",
        window_only: false,
        available_from_timeline: true,
        max_buffer_size: None,
    },
    SupportedEntryType {
        entry_type: "navigation",
        window_only: true,
        available_from_timeline: true,
        max_buffer_size: None,
    },
    SupportedEntryType {
        entry_type: "paint",
        window_only: true,
        available_from_timeline: true,
        max_buffer_size: Some(2),
    },
    SupportedEntryType {
        entry_type: "resource",
        window_only: false,
        available_from_timeline: true,
        max_buffer_size: Some(250),
    },
    SupportedEntryType {
        entry_type: "visibility-state",
        window_only: true,
        available_from_timeline: true,
        max_buffer_size: Some(50),
    },
];

pub(crate) fn supported_entry_types(global_is_window: bool) -> impl Iterator<Item = &'static SupportedEntryType> {
    SUPPORTED_ENTRY_TYPES
        .iter()
        .filter(move |entry_type| global_is_window || !entry_type.window_only)
}
