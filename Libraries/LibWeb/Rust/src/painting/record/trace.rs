/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::order_tree::ProducerKind;
use super::verify::CaptureLog;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paint_order_plan::{PaintScope, PaintScopeKind, StackingContextPaintPhase};
use std::cell::RefCell;
use std::fmt::Write;
use std::rc::Rc;

// The normal recorder carries no observation state. Keeping the policy static also removes
// diagnostic argument construction and branches from its traversal and painter specializations.
pub trait Observer: Clone + Default {
    const ENABLED: bool;
    fn observe(&self, callback: impl FnOnce(&mut CaptureLog));
    fn finish(self) -> Option<CaptureLog>;
}

#[derive(Clone, Default)]
pub struct NoTrace;

impl Observer for NoTrace {
    const ENABLED: bool = false;
    #[inline(always)]
    fn observe(&self, _: impl FnOnce(&mut CaptureLog)) {}
    fn finish(self) -> Option<CaptureLog> {
        None
    }
}

#[derive(Clone, Default)]
pub struct Trace(Rc<RefCell<CaptureLog>>);

impl Observer for Trace {
    const ENABLED: bool = true;
    fn observe(&self, callback: impl FnOnce(&mut CaptureLog)) {
        callback(&mut self.0.borrow_mut());
    }
    fn finish(self) -> Option<CaptureLog> {
        Some(
            Rc::try_unwrap(self.0)
                .expect("nested recording kept its trace alive")
                .into_inner(),
        )
    }
}

/// What a recording did with a scope or producer. A scope is assembled when its published child
/// list is walked, replanned when its plan is rebuilt, recorded when it has no published
/// counterpart and copied when it is clean. A producer is recorded, copied, or skipped when its
/// phase is masked off; an inactive stacking context is skipped as a whole.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Action {
    Assemble,
    Replan,
    Record,
    Copy,
    Skip,
}

#[derive(Clone, Copy, Debug)]
pub(crate) enum Operation {
    Scope(PaintScope),
    Producer(NodeSlotId, ProducerKind),
    // Content recorded inside a producer, or outside the tree: the canvas and inspector overlays.
    Named(Option<NodeSlotId>, &'static str),
}

#[derive(Debug)]
pub(crate) struct Event {
    pub parent: Option<usize>,
    pub operation: Operation,
    pub action: Action,
    pub empty: bool,
}

/// The damage a recording started from, for tests that pin the exact amount of work.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct DamageSummary {
    pub rows: usize,
    pub moved: usize,
    pub order: usize,
    pub eligibility: usize,
    pub all: bool,
}

impl<O: Observer> super::PaintRecorder<'_, O> {
    pub(crate) fn trace_scope<R>(
        &mut self,
        operation: Operation,
        action: Action,
        body: impl FnOnce(&mut Self) -> R,
    ) -> R {
        self.observer.observe(|log| log.begin(operation, action));
        let result = body(self);
        self.observer.observe(|log| log.end(false));
        result
    }

    pub(crate) fn trace_paint(&mut self, operation: Operation, body: impl FnOnce(&mut Self)) {
        self.observer.observe(|log| log.begin(operation, Action::Record));
        let before = if O::ENABLED { self.recorder.byte_size() } else { 0 };
        body(self);
        self.observer
            .observe(|log| log.end(self.recorder.byte_size() == before));
    }
}

impl CaptureLog {
    pub(crate) fn begin(&mut self, operation: Operation, action: Action) {
        let index = self.events.len();
        self.events.push(Event {
            parent: self.open_events.last().copied(),
            operation,
            action,
            empty: false,
        });
        self.open_events.push(index);
    }

    pub(crate) fn end(&mut self, empty: bool) {
        let index = self.open_events.pop().expect("unbalanced recording trace");
        self.events[index].empty = empty;
    }

    pub(crate) fn leaf(&mut self, operation: Operation, action: Action, empty: bool) {
        self.begin(operation, action);
        self.end(empty);
    }

    pub(crate) fn format(&self, mut name: impl FnMut(NodeSlotId) -> String) -> String {
        assert!(self.open_events.is_empty(), "incomplete recording trace");
        let mut output = String::new();
        if let Some(damage) = self.damage {
            writeln!(
                output,
                "damage: rows={} moved={} order={} eligibility={} all={}",
                damage.rows, damage.moved, damage.order, damage.eligibility, damage.all
            )
            .unwrap();
        }
        let mut depths = Vec::with_capacity(self.events.len());
        for event in &self.events {
            let depth = event.parent.map_or(0, |parent| depths[parent] + 1);
            depths.push(depth);
            let label = match event.operation {
                Operation::Scope(scope) => match scope.kind {
                    PaintScopeKind::PaintedAsStackingContext => name(scope.owner),
                    PaintScopeKind::Descendants(phase) => {
                        format!("{}/descendants({})", name(scope.owner), phase_name(phase))
                    }
                },
                Operation::Producer(owner, kind) => format!("{}/{}", name(owner), producer_name(kind)),
                Operation::Named(owner, label) => {
                    owner.map_or_else(|| format!("@{label}"), |owner| format!("{}/{label}", name(owner)))
                }
            };
            let action = match event.action {
                Action::Assemble => "ASSEMBLE",
                Action::Replan => "REPLAN",
                Action::Record => "RECORD",
                Action::Copy => "COPY",
                Action::Skip => "SKIP",
            };
            let empty = if event.empty && event.action == Action::Record {
                " (empty)"
            } else {
                ""
            };
            writeln!(output, "{}{label} {action}{empty}", "  ".repeat(depth)).unwrap();
        }
        output
    }
}

pub(crate) fn producer_name(kind: ProducerKind) -> &'static str {
    match kind {
        ProducerKind::DrawBackground => "background",
        ProducerKind::DrawBorder => "border",
        ProducerKind::DrawTableCollapsedBorder => "table-collapsed-border",
        ProducerKind::DrawForeground => "foreground",
        ProducerKind::DrawOutline => "outline",
        ProducerKind::DrawOverlay => "overlay",
        ProducerKind::HitBackground => "background/hit-test",
        ProducerKind::HitForeground => "foreground/hit-test",
        ProducerKind::HitOverlay => "overlay/hit-test",
        ProducerKind::ScrollMetadata => "scroll-metadata",
        ProducerKind::ScopePreamble => "preamble",
        ProducerKind::Svg => "svg",
        ProducerKind::InlinePiece(_) => "inline-piece",
        ProducerKind::TextFragment(_) => "text-fragment",
    }
}

fn phase_name(phase: StackingContextPaintPhase) -> &'static str {
    use StackingContextPaintPhase::*;
    match phase {
        BackgroundAndBorders => "background-and-borders",
        Floats => "floats",
        BackgroundAndBordersForInlineLevelAndReplaced => "inline-background-and-borders",
        Foreground => "foreground",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn disabled_observation_has_no_state_and_does_not_evaluate_the_callback() {
        assert_eq!(std::mem::size_of::<NoTrace>(), 0);
        NoTrace.observe(|_| panic!("disabled observation ran"));
    }

    #[test]
    fn nested_work_is_indented_and_only_recording_reports_emptiness() {
        let mut log = CaptureLog::default();
        let owner = NodeSlotId::new(0, 1);
        log.damage = Some(DamageSummary {
            rows: 1,
            ..DamageSummary::default()
        });
        log.begin(Operation::Scope(PaintScope::stacking_context(owner)), Action::Assemble);
        log.leaf(
            Operation::Producer(owner, ProducerKind::DrawBackground),
            Action::Copy,
            true,
        );
        log.leaf(
            Operation::Producer(owner, ProducerKind::DrawForeground),
            Action::Record,
            true,
        );
        log.leaf(
            Operation::Producer(owner, ProducerKind::DrawOutline),
            Action::Skip,
            true,
        );
        log.end(false);
        let text = log.format(|_| "box".to_string());
        assert_eq!(
            text,
            "damage: rows=1 moved=0 order=0 eligibility=0 all=false\nbox ASSEMBLE\n  box/background COPY\n  box/foreground RECORD (empty)\n  box/outline SKIP\n"
        );
    }
}
