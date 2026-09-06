/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::LayoutNodeArena;
use super::formatting_context::{FormattingContextType, LayoutMode, LayoutPurpose};
use super::node_data::{NodeKind, NodeSlotId};
use std::cell::RefCell;
use std::ffi::c_void;
use std::fmt::Write;

type AppendText = unsafe extern "C" fn(*mut c_void, *const u8, usize);
type DescribeNode = unsafe extern "C" fn(*mut c_void, *mut c_void, AppendText);

struct Trace {
    describe_node: DescribeNode,
    text: String,
    depth: usize,
}

/// Observation belongs to the document, not to a single pass: geometry reads and
/// style stabilization can cause several passes within one measured mutation.
#[derive(Default)]
pub(crate) struct LayoutTrace(RefCell<Option<Trace>>);

pub(super) struct Scope<'a>(&'a LayoutTrace);

impl Drop for Scope<'_> {
    fn drop(&mut self) {
        self.0.0.borrow_mut().as_mut().unwrap().depth -= 1;
    }
}

impl LayoutTrace {
    fn begin(&self, describe_node: DescribeNode) {
        assert!(self.0.borrow().as_ref().is_none_or(|trace| trace.depth == 0));
        *self.0.borrow_mut() = Some(Trace {
            describe_node,
            text: String::new(),
            depth: 0,
        });
    }

    fn take(&self) -> String {
        let Some(trace) = self.0.borrow_mut().take() else {
            return String::new();
        };
        assert_eq!(trace.depth, 0, "incomplete layout trace");
        trace.text
    }

    fn scope(&self, label: impl FnOnce(DescribeNode) -> String) -> Option<Scope<'_>> {
        let mut state = self.0.borrow_mut();
        let trace = state.as_mut()?;
        // Resolve names while the run's nodes are live. A subsequent mutation may
        // remove them or reuse their arena slots before JavaScript takes the trace.
        writeln!(trace.text, "{}{}", "  ".repeat(trace.depth), label(trace.describe_node)).unwrap();
        trace.depth += 1;
        Some(Scope(self))
    }

    pub(super) fn pass(&self, arena: &LayoutNodeArena, partial_root: Option<NodeSlotId>) -> Option<Scope<'_>> {
        self.scope(|describe| match partial_root {
            Some(root) => format!("layout PARTIAL {}", owner_name(arena, root, describe)),
            None => "layout FULL".into(),
        })
    }

    pub(super) fn run(
        &self,
        arena: &LayoutNodeArena,
        root: NodeSlotId,
        fc_type: FormattingContextType,
        purpose: LayoutPurpose,
        mode: LayoutMode,
        action: impl FnOnce() -> &'static str,
    ) -> Option<Scope<'_>> {
        self.scope(|describe| {
            let owner = owner_name(arena, root, describe);
            let context = match fc_type {
                FormattingContextType::Block => "block",
                FormattingContextType::Inline => "inline",
                FormattingContextType::Flex => "flex",
                FormattingContextType::Grid => "grid",
                FormattingContextType::Table => "table",
                FormattingContextType::Svg => "svg",
                FormattingContextType::ReplacedWithChildren => "replaced-with-children",
                FormattingContextType::InternalReplaced => "internal-replaced",
                FormattingContextType::InternalDummy => "internal-dummy",
            };
            let measurement = match (purpose.is_measurement(), mode) {
                (false, LayoutMode::Normal) => "",
                (false, LayoutMode::IntrinsicSizing) => " (intrinsic)",
                (true, LayoutMode::Normal) => " (measurement)",
                (true, LayoutMode::IntrinsicSizing) => " (measurement, intrinsic)",
            };
            format!("{owner}/{context}{measurement} {}", action())
        })
    }
}

fn owner_name(arena: &LayoutNodeArena, root: NodeSlotId, describe: DescribeNode) -> String {
    if arena.data(root).kind.get() == NodeKind::Viewport {
        return "@viewport".into();
    }
    unsafe extern "C" fn append(sink: *mut c_void, bytes: *const u8, length: usize) {
        // SAFETY: describe receives this live vector and supplies bytes valid for this call.
        unsafe { &mut *sink.cast::<Vec<u8>>() }.extend_from_slice(unsafe { std::slice::from_raw_parts(bytes, length) });
    }
    let mut bytes = Vec::<u8>::new();
    // SAFETY: the traced run holds the arena and its shells alive; describe copies
    // the node's description synchronously without changing layout.
    unsafe { describe(arena.shell_if_live(root), (&raw mut bytes).cast(), append) };
    String::from_utf8(bytes).expect("layout trace label must be UTF-8")
}

/// # Safety
/// The arena must be live. The callback must remain valid until tracing stops and
/// must synchronously describe its live node shell without mutating layout.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_begin_layout_trace(arena: *mut c_void, describe_node: DescribeNode) {
    unsafe { LayoutNodeArena::from_handle(arena) }
        .layout_trace
        .begin(describe_node);
}

/// # Safety
/// The arena must be live, and append_text must synchronously copy the supplied bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_take_layout_trace(
    arena: *mut c_void,
    context: *mut c_void,
    append_text: AppendText,
) {
    let text = unsafe { LayoutNodeArena::from_handle(arena) }.layout_trace.take();
    unsafe { append_text(context, text.as_ptr(), text.len()) };
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe extern "C" fn unused_description(_: *mut c_void, _: *mut c_void, _: AppendText) {
        panic!("no nodes to describe in this test");
    }

    #[test]
    fn disabled_trace_does_not_construct_labels() {
        let trace = LayoutTrace::default();
        assert!(trace.scope(|_| panic!("disabled observation")).is_none());
        assert_eq!(trace.take(), "");
    }

    #[test]
    fn preserves_nesting_repeated_runs_and_multiple_passes() {
        let trace = LayoutTrace::default();
        trace.begin(unused_description);
        {
            let _pass = trace.scope(|_| "layout FULL".into());
            let _run = trace.scope(|_| "@viewport/block RUN (cache=bypass)".into());
            {
                let _child = trace.scope(|_| "#child/block REUSE SUBTREE".into());
            }
            let _child = trace.scope(|_| "#child/block RUN (cache=miss)".into());
        }
        {
            let _pass = trace.scope(|_| "layout PARTIAL #boundary".into());
        }
        assert_eq!(
            trace.take(),
            "layout FULL\n  @viewport/block RUN (cache=bypass)\n    #child/block REUSE SUBTREE\n    #child/block RUN (cache=miss)\nlayout PARTIAL #boundary\n"
        );
        assert!(trace.scope(|_| panic!("take must disable tracing")).is_none());
        assert_eq!(trace.take(), "");
    }

    #[test]
    fn begin_discards_previous_events() {
        let trace = LayoutTrace::default();
        trace.begin(unused_description);
        drop(trace.scope(|_| "old pass".into()));
        trace.begin(unused_description);
        assert_eq!(trace.take(), "");
    }
}
