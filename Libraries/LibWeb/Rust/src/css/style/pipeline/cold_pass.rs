/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A pass styling the whole document from scratch, for a test to read what it measures and whether
//! what the engine holds differs from what it computes. Nothing it computes is installed.

use super::super::{RetainedState, StyleEngine, StyleNodeID};
use super::self_check;

/// What the last cold pass measured, by `ReportField`.
pub(in super::super) type Report = [u64; ReportField::COUNT];

#[derive(Clone, Copy)]
#[repr(u32)]
pub(in super::super) enum ReportField {
    Elements,
    IndexedEntries,
    IndexNanoseconds,
    StyleNanoseconds,
    Threads,
    Candidates,
    BloomRejections,
    Evaluations,
    Matches,
    Winners,
    ComputedElements,
    DirectlyComputedElements,
    SharedElements,
    ReusedElements,
    PseudoElements,
    Differences,
}

impl ReportField {
    pub(in super::super) const COUNT: usize = Self::Differences as usize + 1;
}

impl StyleEngine {
    /// Style the document tree under `root` from scratch on `threads` threads, or as many as the
    /// machine has for none, count what differs from what the engine holds when asked to, and
    /// report the time the styling took in nanoseconds.
    pub(in super::super) fn run_cold_pass(&mut self, root: StyleNodeID, verify: bool, threads: u32) -> u64 {
        let Some(pass) = self_check::reference_pass(self, root, true, threads) else {
            return 0;
        };
        let statistics = &pass.statistics;
        let outcomes = &statistics.outcomes;
        let mut report = [0_u64; ReportField::COUNT];
        let mut set = |field: ReportField, value: u64| report[field as usize] = value;
        set(ReportField::Elements, outcomes.elements());
        set(ReportField::IndexedEntries, pass.indexed_entries);
        set(ReportField::IndexNanoseconds, pass.index_nanoseconds);
        set(ReportField::StyleNanoseconds, pass.style_nanoseconds);
        set(ReportField::Threads, pass.threads);
        set(ReportField::Candidates, statistics.match_counters.candidates);
        set(ReportField::BloomRejections, statistics.match_counters.bloom_rejections);
        set(ReportField::Evaluations, statistics.match_counters.evaluations);
        set(ReportField::Matches, statistics.match_counters.matches);
        set(ReportField::Winners, statistics.cascade_counters.winners);
        set(ReportField::ComputedElements, outcomes.styled());
        set(ReportField::DirectlyComputedElements, outcomes.computed_directly());
        set(ReportField::SharedElements, outcomes.shared());
        set(ReportField::ReusedElements, outcomes.reused());
        set(
            ReportField::PseudoElements,
            self.retained
                .projection
                .style_preorder(root)
                .map(|node| pass.records.pseudo_element_records(node).len() as u64)
                .sum(),
        );
        let nanoseconds = pass.index_nanoseconds + pass.style_nanoseconds;
        if verify {
            report[ReportField::Differences as usize] = self.self_check_failures(root, None).len() as u64;
        }
        self.retained.cold_pass_report = report;
        nanoseconds
    }
}

impl RetainedState {
    /// The value of one field of the last cold pass's report, or zero past the last field.
    pub(in super::super) fn cold_pass_report(&self, index: u32) -> u64 {
        self.cold_pass_report.get(index as usize).copied().unwrap_or(0)
    }
}
