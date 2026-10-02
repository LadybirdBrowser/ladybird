/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::LayoutNodeArena;
use super::formatting_context::{FormattingContextType, LayoutMode, LayoutPurpose};
use super::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::stage::MainThread;
use std::cell::RefCell;
use std::ffi::c_void;
use std::fmt::Write;

type AppendText = unsafe extern "C" fn(*mut c_void, *const u8, usize);
pub(crate) type DescribeNode = unsafe extern "C" fn(*mut c_void, *mut c_void, AppendText);

struct Trace {
    lines: Vec<Line>,
    depth: usize,
}

/// One traced event: what it says, and the box it names, if any. The box is named once the pass is
/// over, since naming it asks the document, which a pass cannot do.
struct Line {
    depth: usize,
    prefix: &'static str,
    owner: Option<NodeSlotId>,
    owner_name: Option<String>,
    text: String,
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
    fn begin(&self) {
        assert!(self.0.borrow().as_ref().is_none_or(|trace| trace.depth == 0));
        *self.0.borrow_mut() = Some(Trace {
            lines: Vec::new(),
            depth: 0,
        });
    }

    fn take(&self) -> String {
        let Some(trace) = self.0.borrow_mut().take() else {
            return String::new();
        };
        assert_eq!(trace.depth, 0, "incomplete layout trace");
        let mut text = String::new();
        for line in trace.lines {
            writeln!(
                text,
                "{}{}{}{}",
                "  ".repeat(line.depth),
                line.prefix,
                line.owner_name.unwrap_or_default(),
                line.text
            )
            .unwrap();
        }
        text
    }

    /// Names the boxes the traced events name. This runs once the outermost pass is over, while
    /// the boxes the pass ran for are still live: a later mutation may remove them or reuse their
    /// arena slots before JavaScript takes the trace. The host describes them through the callback
    /// it registered when tracing began.
    pub(super) fn name_owners(&self, main_thread: &MainThread, arena: &LayoutNodeArena) {
        let mut state = self.0.borrow_mut();
        let Some(trace) = state.as_mut() else {
            return;
        };
        let mut describe = None;
        for line in &mut trace.lines {
            let Some(owner) = line.owner.filter(|_| line.owner_name.is_none()) else {
                continue;
            };
            let describe = *describe.get_or_insert_with(|| {
                main_thread
                    .host_tables()
                    .and_then(|host_tables| host_tables.layout_trace_describe_node.get())
                    .expect("a layout trace names its boxes through the callback it began with")
            });
            line.owner_name = Some(owner_name(arena, owner, describe));
        }
    }

    fn scope(
        &self,
        prefix: &'static str,
        owner: Option<NodeSlotId>,
        text: impl FnOnce() -> String,
    ) -> Option<Scope<'_>> {
        let mut state = self.0.borrow_mut();
        let trace = state.as_mut()?;
        trace.lines.push(Line {
            depth: trace.depth,
            prefix,
            owner,
            owner_name: None,
            text: text(),
        });
        trace.depth += 1;
        Some(Scope(self))
    }

    pub(super) fn pass(&self, partial_root: Option<NodeSlotId>) -> Option<Scope<'_>> {
        match partial_root {
            Some(root) => self.scope("layout PARTIAL ", Some(root), String::new),
            None => self.scope("layout FULL", None, String::new),
        }
    }

    pub(super) fn run(
        &self,
        root: NodeSlotId,
        fc_type: FormattingContextType,
        purpose: LayoutPurpose,
        mode: LayoutMode,
        action: impl FnOnce() -> &'static str,
    ) -> Option<Scope<'_>> {
        self.scope("", Some(root), || {
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
            format!("/{context}{measurement} {}", action())
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
    // A row nothing has made a shell for is named from the row, the way its shell would describe
    // itself.
    let data = arena.data(root);
    if data.shell.get().is_null() {
        let kind = data.kind.get();
        if data.flags.get() & NodeFlag::Anonymous as u32 != 0 {
            return format!("{kind:?}(anonymous)");
        }
        if kind == NodeKind::TextNode {
            return format!("{kind:?}<#text>");
        }
    }
    let mut bytes = Vec::<u8>::new();
    // SAFETY: The pass is over, and the rows it ran for are live; describe copies the node's
    // description synchronously without changing layout.
    unsafe { describe(arena.shell_if_live(root), (&raw mut bytes).cast(), append) };
    String::from_utf8(bytes).expect("layout trace label must be UTF-8")
}

/// # Safety
/// The arena must be live. The callback must remain valid until tracing stops and
/// must synchronously describe its live node shell without mutating layout.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_begin_layout_trace(arena: *mut c_void, describe_node: DescribeNode) {
    unsafe { super::HostTables::from_handle(arena) }
        .layout_trace_describe_node
        .set(Some(describe_node));
    unsafe { LayoutNodeArena::from_handle(arena) }.layout_trace.begin();
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

    #[test]
    fn disabled_trace_does_not_construct_labels() {
        let trace = LayoutTrace::default();
        assert!(trace.scope("", None, || panic!("disabled observation")).is_none());
        assert_eq!(trace.take(), "");
    }

    #[test]
    fn preserves_nesting_repeated_runs_and_multiple_passes() {
        let trace = LayoutTrace::default();
        trace.begin();
        {
            let _pass = trace.scope("layout FULL", None, String::new);
            let _run = trace.scope("@viewport/block RUN (cache=bypass)", None, String::new);
            {
                let _child = trace.scope("#child/block REUSE SUBTREE", None, String::new);
            }
            let _child = trace.scope("#child/block RUN (cache=miss)", None, String::new);
        }
        {
            let _pass = trace.scope("layout PARTIAL #boundary", None, String::new);
        }
        assert_eq!(
            trace.take(),
            "layout FULL\n  @viewport/block RUN (cache=bypass)\n    #child/block REUSE SUBTREE\n    #child/block RUN (cache=miss)\nlayout PARTIAL #boundary\n"
        );
        assert!(trace.scope("", None, || panic!("take must disable tracing")).is_none());
        assert_eq!(trace.take(), "");
    }

    #[test]
    fn begin_discards_previous_events() {
        let trace = LayoutTrace::default();
        trace.begin();
        drop(trace.scope("old pass", None, String::new));
        trace.begin();
        assert_eq!(trace.take(), "");
    }

    #[test]
    fn owners_are_named_through_the_registered_callback_once_the_pass_is_over() {
        unsafe extern "C" fn describe(_: *mut c_void, sink: *mut c_void, append: AppendText) {
            let name = b"Box<div>#owner";
            // SAFETY: The trace hands a live sink and its append function.
            unsafe { append(sink, name.as_ptr(), name.len()) };
        }
        let mut arena = LayoutNodeArena::new();
        let owner = arena.allocate_for_test();
        arena.data(owner.slot).kind.set(NodeKind::BlockContainer);
        let host_tables = crate::layout::HostTables::default();
        host_tables.layout_trace_describe_node.set(Some(describe));
        let main_thread = MainThread::for_test_with_host(&host_tables);

        arena.layout_trace.begin();
        arena.begin_active_layout_pass();
        drop(arena.layout_trace.run(
            owner.slot,
            FormattingContextType::Block,
            LayoutPurpose::Commit,
            LayoutMode::Normal,
            || "RUN",
        ));
        arena.end_active_layout_pass(&main_thread);
        assert_eq!(arena.layout_trace.take(), "Box<div>#owner/block RUN\n");
        arena
            .free_subtree(owner.slot)
            .destroy_shells_and_invoke_callbacks(&main_thread);
    }
}
