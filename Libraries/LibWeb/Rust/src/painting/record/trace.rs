/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::PaintPhase;
use super::cache::{CaptureKind, CaptureSite};
use super::verify::CaptureLog;
use crate::layout::node_data::NodeSlotId;
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Action {
    Walk,
    Record,
    Reuse,
    Skip,
}

#[derive(Clone, Copy, Debug)]
pub(crate) enum Operation {
    Capture(CaptureSite),
    HitTest(NodeSlotId, PaintPhase),
    Producer(Option<NodeSlotId>, &'static str),
}

#[derive(Debug)]
pub(crate) struct Event {
    pub parent: Option<usize>,
    pub operation: Operation,
    pub action: Action,
    pub empty: bool,
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
        let mut depths = Vec::with_capacity(self.events.len());
        for event in &self.events {
            let depth = event.parent.map_or(0, |parent| depths[parent] + 1);
            depths.push(depth);
            let label = match event.operation {
                Operation::Capture(site) => match site.kind {
                    CaptureKind::PaintedAsStackingContext => name(site.paintable),
                    CaptureKind::DescendantSubtreePhase(phase) => {
                        format!("{}/descendants({})", name(site.paintable), phase_name(phase))
                    }
                    CaptureKind::BoxPhase(phase) => format!("{}/{}", name(site.paintable), box_phase_name(phase)),
                },
                Operation::HitTest(owner, phase) => format!("{}/{}/hit-test", name(owner), box_phase_name(phase)),
                Operation::Producer(owner, label) => {
                    owner.map_or_else(|| format!("@{label}"), |owner| format!("{}/{label}", name(owner)))
                }
            };
            let action = match event.action {
                Action::Walk => "WALK",
                Action::Record => "RECORD",
                Action::Reuse => match event.operation {
                    Operation::Capture(CaptureSite {
                        kind: CaptureKind::PaintedAsStackingContext | CaptureKind::DescendantSubtreePhase(_),
                        ..
                    }) => "REUSE WHOLE CAPTURE",
                    _ => "REUSE",
                },
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

fn box_phase_name(phase: PaintPhase) -> &'static str {
    match phase {
        PaintPhase::Background => "background",
        PaintPhase::Border => "border",
        PaintPhase::TableCollapsedBorder => "table-collapsed-border",
        PaintPhase::Foreground => "foreground",
        PaintPhase::Outline => "outline",
        PaintPhase::Overlay => "overlay",
    }
}

fn phase_name(phase: super::traversal::StackingContextPaintPhase) -> &'static str {
    use super::traversal::StackingContextPaintPhase::*;
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
    fn reuse_is_a_leaf_and_empty_painting_is_work() {
        let mut log = CaptureLog::default();
        let root = CaptureSite {
            paintable: NodeSlotId::new(0, 1),
            kind: CaptureKind::PaintedAsStackingContext,
        };
        log.begin(Operation::Capture(root), Action::Walk);
        log.leaf(
            Operation::Capture(CaptureSite {
                kind: CaptureKind::BoxPhase(PaintPhase::Foreground),
                ..root
            }),
            Action::Record,
            true,
        );
        log.leaf(
            Operation::Capture(CaptureSite {
                paintable: NodeSlotId::new(1, 1),
                ..root
            }),
            Action::Reuse,
            false,
        );
        log.end(false);
        assert_eq!(
            log.format(|node| if node == root.paintable {
                "@viewport".into()
            } else {
                "#sibling".into()
            }),
            "@viewport WALK\n  @viewport/foreground RECORD (empty)\n  #sibling REUSE WHOLE CAPTURE\n"
        );
    }
}
