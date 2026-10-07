/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::LayoutNodeArena;
use super::formatting_context::{FormattingContextType, LayoutMode, LayoutPurpose};
use super::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::render_state::DocumentHost;
use crate::stage::MainThread;
use std::cell::RefCell;
use std::ffi::c_void;
use std::fmt::Write;

type AppendText = unsafe extern "C" fn(*mut c_void, *const u8, usize);
/// Describes the row in the slot of the document whose host it is handed, as the row's layout node describes itself.
pub(crate) type DescribeNode =
    unsafe extern "C" fn(*const DocumentHost, &crate::render_state::BegunRead, NodeSlotId, *mut c_void, AppendText);

#[derive(Clone)]

struct Trace {
    lines: Vec<Line>,
    depth: usize,
}

/// One traced event: what it says, and the box it names, if any. The box is named once the pass is
/// over, since naming it asks the document, which a pass cannot do.
#[derive(Clone)]
struct Line {
    depth: usize,
    prefix: &'static str,
    owner: Option<NodeSlotId>,
    owner_name: Option<String>,
    text: String,
}

/// Observation belongs to the document, not to a single pass: geometry reads and
/// style stabilization can cause several passes within one measured mutation.
#[derive(Clone, Default)]
pub(crate) struct LayoutTrace(RefCell<Option<Trace>>);

pub(super) struct Scope<'a>(&'a LayoutTrace);

impl Drop for Scope<'_> {
    fn drop(&mut self) {
        self.0.0.borrow_mut().as_mut().unwrap().depth -= 1;
    }
}

impl LayoutTrace {
    pub(super) fn begin(&self) {
        assert!(self.0.borrow().as_ref().is_none_or(|trace| trace.depth == 0));
        *self.0.borrow_mut() = Some(Trace {
            lines: Vec::new(),
            depth: 0,
        });
    }

    pub(super) fn take(&self) -> String {
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

    /// The boxes the traced events name and that are not named yet, each by its line. A box whose row says what it is
    /// is named from the row; the host describes the rest.
    fn unnamed_owners(&self, arena: &LayoutNodeArena) -> Vec<UnnamedOwner> {
        let state = self.0.borrow();
        let Some(trace) = state.as_ref() else {
            return Vec::new();
        };
        trace
            .lines
            .iter()
            .enumerate()
            .filter(|(_, line)| line.owner_name.is_none())
            .filter_map(|(line, Line { owner, .. })| {
                let row = (*owner)?;
                Some(UnnamedOwner {
                    line,
                    row,
                    name: name_from_row(arena, row),
                })
            })
            .collect()
    }

    /// Names the boxes of the traced events at the lines `names` gives.
    fn name_owners(&self, names: Vec<(usize, String)>) {
        let mut state = self.0.borrow_mut();
        let Some(trace) = state.as_mut() else {
            return;
        };
        for (line, name) in names {
            trace.lines[line].owner_name = Some(name);
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

/// A box a traced event names that is not named yet: the event's line, its row, and its name where the row says what
/// it is.
struct UnnamedOwner {
    line: usize,
    row: NodeSlotId,
    name: Option<String>,
}

/// The name of the box in `root` where its row says what it is: the viewport, or an anonymous or text row, which is
/// named the way its layout node describes itself, which saves making one.
fn name_from_row(arena: &LayoutNodeArena, root: NodeSlotId) -> Option<String> {
    let data = arena.data(root);
    let kind = data.kind.get();
    if kind == NodeKind::Viewport {
        return Some("@viewport".into());
    }
    if data.flags.get() & NodeFlag::Anonymous as u32 != 0 {
        return Some(format!("{kind:?}(anonymous)"));
    }
    (kind == NodeKind::TextNode).then(|| format!("{kind:?}<#text>"))
}

/// Has a running layout trace of `host`'s document name the boxes the passes it traced ran for. This runs once the
/// outermost pass is over, while the boxes the pass ran for are still live: a later mutation may remove them or reuse
/// their arena slots before JavaScript takes the trace. The host describes them through the callback it registered
/// when tracing began. Nothing happens without a trace.
pub(crate) fn name_layout_trace_owners(main_thread: &MainThread, read: &crate::render_state::BegunRead) {
    let Some(host) = main_thread.host() else {
        return;
    };
    // Only a trace that began registered the callback, so the host knows a document never traced has nothing to name.
    let Some(describe) = host.host_tables().layout_trace_describe_node.get() else {
        return;
    };
    // SAFETY: The host is live for the token's entry.
    let owners =
        unsafe { super::shell_reads::read_arena(host, read, (), |arena, ()| arena.layout_trace.unnamed_owners(arena)) };
    if owners.is_empty() {
        return;
    }
    let names = owners
        .into_iter()
        .map(|owner| {
            (
                owner.line,
                owner
                    .name
                    .unwrap_or_else(|| describe_node(host, read, owner.row, describe)),
            )
        })
        .collect();
    // SAFETY: As above.
    unsafe { super::layout_changes::queue(host, super::layout_changes::LayoutChange::NameLayoutTraceOwners(names)) };
}

fn describe_node(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    row: NodeSlotId,
    describe: DescribeNode,
) -> String {
    unsafe extern "C" fn append(sink: *mut c_void, bytes: *const u8, length: usize) {
        // SAFETY: describe receives this live vector and supplies bytes valid for this call.
        unsafe { &mut *sink.cast::<Vec<u8>>() }.extend_from_slice(unsafe { std::slice::from_raw_parts(bytes, length) });
    }
    let mut bytes = Vec::<u8>::new();
    // SAFETY: The pass is over, and the rows it ran for are live; describe copies the row's description synchronously
    // without changing layout.
    unsafe { describe(host, read, row, (&raw mut bytes).cast(), append) };
    String::from_utf8(bytes).expect("layout trace label must be UTF-8")
}

impl LayoutNodeArena {
    /// Names the boxes of the traced events at the lines `names` gives.
    pub(crate) fn name_layout_trace_owners(&self, names: Vec<(usize, String)>) {
        self.layout_trace.name_owners(names);
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread. The callback must remain valid until tracing stops
/// and must synchronously describe the live row it is handed without mutating layout.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_begin_layout_trace(host: &DocumentHost, describe_node: DescribeNode) {
    host.host_tables().layout_trace_describe_node.set(Some(describe_node));
    // SAFETY: As above.
    unsafe { super::layout_changes::queue(host, super::layout_changes::LayoutChange::BeginLayoutTrace) };
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `append_text` must synchronously copy the
/// supplied bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_take_layout_trace(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    context: *mut c_void,
    append_text: AppendText,
) {
    // SAFETY: Guaranteed by the caller.
    let text = unsafe { super::shell_reads::read_arena(host, read, (), |arena, ()| arena.layout_trace.take()) };
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
        unsafe extern "C" fn describe(
            _: *const DocumentHost,
            _: &crate::render_state::BegunRead,
            _: NodeSlotId,
            sink: *mut c_void,
            append: AppendText,
        ) {
            let name = b"Box<div>#owner";
            // SAFETY: The trace hands a live sink and its append function.
            unsafe { append(sink, name.as_ptr(), name.len()) };
        }
        let mut arena = LayoutNodeArena::new();
        let owner = arena.allocate_for_test();
        arena.write_shape(owner.slot).set_kind(NodeKind::BlockContainer);
        let host = crate::render_state::DocumentHost::for_test();
        let host_tables = host.host_tables();
        host_tables.layout_trace_describe_node.set(Some(describe));
        let main_thread = MainThread::for_test_with_host(&host);

        arena.layout_trace.begin();
        arena.begin_active_layout_pass();
        drop(arena.layout_trace.run(
            owner.slot,
            FormattingContextType::Block,
            LayoutPurpose::Commit,
            LayoutMode::Normal,
            || "RUN",
        ));
        assert!(arena.leave_active_layout_pass());
        let names = arena
            .layout_trace
            .unnamed_owners(&arena)
            .into_iter()
            .map(|owner| {
                (
                    owner.line,
                    owner
                        .name
                        .unwrap_or_else(|| describe_node(&host, host.read_for_test(), owner.row, describe)),
                )
            })
            .collect();
        arena.name_layout_trace_owners(names);
        assert_eq!(arena.layout_trace.take(), "Box<div>#owner/block RUN\n");
        arena
            .free_subtree(owner.slot)
            .destroy_shells_and_invoke_callbacks(&main_thread);
    }
}
