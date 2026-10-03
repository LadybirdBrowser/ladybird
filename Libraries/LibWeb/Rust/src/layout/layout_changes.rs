/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host writes of a document's layout, as typed changes its render state applies to the arena. The host names
//! a node by its slot or its identity and queues the change on the document's host; it does not reach the arena.

use super::LayoutNodeArena;
use super::node_data::{NodeFlag, NodeSlotId};
use super::svg_formatting_context::{FfiFloatPoint, FfiSvgAttributeFacts};
use super::tree_mutation::{HostCalls, HostWorkDue, OwedHostWork};
use super::used_values::FfiCssPixelPoint;
use crate::css::css_pixels::CssPixelPoint;
use crate::css::style::NaturalSize;
use crate::css::style::tree::StyleNodeID;
use crate::painting::host::FfiNaturalSize;
use crate::render_state::{ArenaChange, DocumentHost, RenderWait, ask};
use smallvec::SmallVec;

/// One write of the host to a document's layout marks or layout facts, which the render state applies to the arena
/// before anything that reads it.
pub(crate) enum LayoutChange {
    SetNeedsLayoutUpdate {
        node: NodeSlotId,
        propagate_through_ancestors: bool,
    },
    SetNeedsFullLayoutTreeUpdate(bool),
    /// What the node's content is sized from changed: its fragment caches and intrinsic sizes, and those of its
    /// ancestors, are stale.
    ResetCachedIntrinsicSizesOfSelfAndAncestors {
        node: NodeSlotId,
    },
    /// What an insertion under `parent` invalidates depends on the boxes it attaches, which only the layout tree build
    /// knows.
    DeferChildListInsertionLayoutUpdate {
        parent: NodeSlotId,
    },
    /// The text node's data changed.
    InvalidateTextContent {
        node: NodeSlotId,
    },
    /// The text under `root` renders with the language it now resolves: where any of it is cased by its language, the
    /// root lays out again.
    EnrollTextAfterLanguageChange {
        root: NodeSlotId,
    },
    RecordPartialRelayoutEscape,
    /// The absolutely positioned `child`, whose containing block is `parent`, is about to be removed.
    NoteContainedAbsposChildRemoval {
        parent: NodeSlotId,
        child: NodeSlotId,
    },
    /// The identity `node` names was handed to a node, which holds no layout tree update mark, whatever the node that
    /// held it before left behind.
    ClearLayoutTreeUpdateMarks(StyleNodeID),
    /// The DOM node identified by `old` took `new`. Its rows, and those of the pseudo-elements `generated_for` lists,
    /// take the new identity along with their bindings; the old one leaves every row carrying it, and the new one
    /// leaves the layout tree update marks its previous holder left.
    StyleNodeChanged {
        old: Option<StyleNodeID>,
        new: Option<StyleNodeID>,
        generated_for: SmallVec<[u8; 4]>,
    },
    /// The elements registered under one anchor name in one tree scope, in tree order. The scope is named by the
    /// identity of its shadow host, or none for the document tree; no element forgets the name.
    SetAnchorNameElements {
        scope_host: Option<StyleNodeID>,
        anchor_name: usize,
        elements: Box<[StyleNodeID]>,
    },
    /// What the element has scrolled to.
    SetElementScrollOffset {
        element: StyleNodeID,
        offset: CssPixelPoint,
    },
    /// What the pseudo-element of kind `generated_for` on `generator` has scrolled to.
    SetPseudoElementScrollOffset {
        generator: StyleNodeID,
        generated_for: u8,
        offset: CssPixelPoint,
    },
    /// Whether the node sits in the user agent shadow tree of the focused text control.
    SetIdentityInFocusedTextControl {
        node: StyleNodeID,
        value: bool,
    },
    /// What the SVG element's presentation attributes parse to, with the points of a `<polyline>` or `<polygon>`.
    SvgAttributeFacts {
        element: StyleNodeID,
        facts: Box<FfiSvgAttributeFacts>,
        points: Box<[FfiFloatPoint]>,
    },
    /// The resources an SVG graphics element's style names: its mask, clip path, fill and stroke.
    SvgStyleReferences {
        element: StyleNodeID,
        references: [u32; 4],
    },
    /// Whether the document is an SVG file decoded as an image, which the layout of its SVG roots reads.
    SetDocumentIsDecodedSvg(bool),
    /// A fact of the node's DOM node the host stamps into its row.
    SetNodeFlag {
        node: NodeSlotId,
        flag: NodeFlag,
        value: bool,
    },
    /// The image the image box's own provider shows is of this natural size now.
    SetOwnedImageNaturalSize {
        node: NodeSlotId,
        natural_size: NaturalSize,
    },
    /// A layout committed: the text find in page searches is mapped again.
    InvalidateSearchableText,
    /// The element the row was built for published other table spans: the row takes them, and lays out again where
    /// they changed.
    RestampTableSpans {
        node: NodeSlotId,
    },
    /// The row `added` was built for the DOM node the row `bound` is bound to.
    NoteRowsShareDomNode {
        bound: NodeSlotId,
        added: NodeSlotId,
    },
    /// The row is bound to the node it was built for.
    BindRow(NodeSlotId),
    /// The row is no longer bound to the node it was built for.
    UnbindRow(NodeSlotId),
    /// Whether the row needs a frame of the compositor's animation of `kind`.
    SetNodeNeedsCompositorAnimationFrame {
        node: NodeSlotId,
        kind: super::node_data::CompositorAnimationFrameKind,
        value: bool,
    },
    /// The row takes the style record `record`, whose payloads `payloads` holds.
    SetNodeStyle {
        node: NodeSlotId,
        record: u64,
        payloads: super::node_data::StylePayloadsRef,
    },
    /// The style record of the box the node `style_node` is bound to, or of the box of its pseudo-element
    /// `generated_for`, stays readable once the node has left the document.
    PinBoundBoxStyleRecordForDetachment {
        style_node: StyleNodeID,
        generated_for: u8,
    },
    /// The host's readers of the row hold the style record `record` until the host releases it or the row is freed.
    PinNodeStyleRecordForHost {
        node: NodeSlotId,
        record: u64,
    },
    /// The host's readers of the row let go of the style record they held.
    ReleaseNodeStyleRecordPinForHost {
        node: NodeSlotId,
    },
    /// How the host learns what boxes a DOM node has, or none.
    SetBoxPresenceHost(Option<super::layout_node_arena::BoxPresenceHost>),
    /// The text content and replaced-content facts of every node enrolled since the last sync are refreshed.
    SyncEnrolledContentForLayout,
    /// The document's layout passes are traced from now on.
    BeginLayoutTrace,
    /// The boxes of the traced events at these lines are named so.
    NameLayoutTraceOwners(Vec<(usize, String)>),
    /// The tree scope registers these counter styles now.
    CounterStyles {
        tree_scope: u32,
        scope: crate::css::counter_representation::CounterStyleScope,
    },
}

impl LayoutChange {
    /// Applies the change to `arena`, the arena of the document it was queued for. A node freed since then has nothing
    /// left to change.
    pub(crate) fn apply(self, arena: &mut LayoutNodeArena) {
        match self {
            Self::SetNeedsLayoutUpdate {
                node,
                propagate_through_ancestors,
            } => {
                if arena.slot_is_live(node) {
                    arena.set_needs_layout_update(node, propagate_through_ancestors);
                }
            }
            Self::SetNeedsFullLayoutTreeUpdate(value) => arena.set_needs_full_layout_tree_update(value),
            Self::ResetCachedIntrinsicSizesOfSelfAndAncestors { node } => {
                if arena.slot_is_live(node) {
                    arena.bump_fragment_cache_epoch_of_self_and_ancestors(node);
                    arena.reset_cached_intrinsic_sizes_of_self_and_ancestors(node);
                }
            }
            Self::DeferChildListInsertionLayoutUpdate { parent } => {
                if arena.slot_is_live(parent) {
                    arena.defer_child_list_insertion_layout_update(parent);
                }
            }
            Self::InvalidateTextContent { node } => {
                if arena.slot_is_live(node) {
                    arena.invalidate_text_content(node);
                }
            }
            Self::EnrollTextAfterLanguageChange { root } => {
                if arena.slot_is_live(root) && super::rendered_text::enroll_text_after_language_change(arena, root) {
                    arena.set_needs_layout_update(root, true);
                }
            }
            Self::RecordPartialRelayoutEscape => arena.record_partial_relayout_escape(),
            Self::NoteContainedAbsposChildRemoval { parent, child } => {
                if arena.slot_is_live(parent) && arena.slot_is_live(child) {
                    arena.note_contained_abspos_child_removal(parent, child);
                }
            }
            Self::ClearLayoutTreeUpdateMarks(node) => arena.layout_tree_update_marks().borrow_mut().clear(node),
            Self::StyleNodeChanged {
                old,
                new,
                generated_for,
            } => {
                if let (Some(old), Some(new)) = (old, new) {
                    arena.move_bound_rows_to_style_node(old, new);
                    for generated_for in generated_for {
                        arena.move_bound_pseudo_element_rows_to_style_node(old, generated_for, new);
                    }
                }
                if let Some(old) = old {
                    arena.forget_style_node(old);
                }
                arena.clear_layout_tree_update_marks(new);
            }
            Self::SetAnchorNameElements {
                scope_host,
                anchor_name,
                elements,
            } => arena.set_anchor_name_elements(scope_host, anchor_name, elements.iter().copied()),
            Self::SetElementScrollOffset { element, offset } => arena.set_element_scroll_offset(element, offset),
            Self::SetPseudoElementScrollOffset {
                generator,
                generated_for,
                offset,
            } => arena.set_pseudo_element_scroll_offset(generator, generated_for, offset),
            Self::SetIdentityInFocusedTextControl { node, value } => {
                arena.set_identity_in_focused_text_control(node, value);
            }
            Self::SvgAttributeFacts { element, facts, points } => {
                arena.set_style_node_svg_attribute_facts(element, *facts, &points);
            }
            Self::SvgStyleReferences { element, references } => {
                arena.set_style_node_svg_style_references(element, references);
            }
            Self::SetDocumentIsDecodedSvg(is_decoded_svg) => arena.set_document_is_decoded_svg(is_decoded_svg),
            Self::SetNodeFlag { node, flag, value } => {
                if arena.slot_is_live(node) {
                    arena.set_node_flag(node, flag, value);
                }
            }
            Self::SetOwnedImageNaturalSize { node, natural_size } => {
                if arena.slot_is_live(node) {
                    arena.set_owned_image_natural_size(node, natural_size);
                }
            }
            Self::InvalidateSearchableText => arena.invalidate_searchable_text(),
            Self::RestampTableSpans { node } => {
                if arena.slot_is_live(node) && arena.restamp_table_spans(node) {
                    arena.set_needs_layout_update(node, true);
                }
            }
            Self::NoteRowsShareDomNode { bound, added } => arena.note_rows_share_dom_node(bound, added),
            Self::BindRow(node) => arena.bind_row(node),
            Self::UnbindRow(node) => arena.unbind_row(node),
            Self::SetNodeNeedsCompositorAnimationFrame { node, kind, value } => {
                arena.set_node_needs_compositor_animation_frame(node, kind, value);
            }
            Self::SetNodeStyle { node, record, payloads } => {
                if arena.set_node_style(node, record, payloads) {
                    arena.refresh_style_flags(node);
                }
                arena.publish_new_size_container_geometry(node);
                arena.enroll_node_for_svg_paint_resources_sync(node);
            }
            Self::PinBoundBoxStyleRecordForDetachment {
                style_node,
                generated_for,
            } => {
                let row = if generated_for == 0 {
                    arena.bound_row(style_node)
                } else {
                    arena.bound_pseudo_element_row(style_node, generated_for)
                };
                if !row.is_invalid() {
                    arena.pin_style_record_for_detachment(row);
                }
            }
            Self::PinNodeStyleRecordForHost { node, record } => arena.pin_node_style_record_for_host(node, record),
            Self::ReleaseNodeStyleRecordPinForHost { node } => arena.release_node_style_record_pin_for_host(node),
            Self::SetBoxPresenceHost(host) => arena.set_box_presence_host(host),
            Self::SyncEnrolledContentForLayout => super::layout_node_arena::sync_enrolled_content_for_layout(arena),
            Self::BeginLayoutTrace => arena.layout_trace.begin(),
            Self::NameLayoutTraceOwners(names) => arena.name_layout_trace_owners(names),
            Self::CounterStyles { tree_scope, scope } => arena.publish_counter_styles(tree_scope, scope),
        }
    }
}

/// A write the host waits for the render state to make, as it pays what the write owes it before it goes on.
#[derive(Clone, Copy, Debug)]
pub(crate) enum LayoutWrite {
    /// Detaches the layout subtree `root` heads from its parent, if it has one, and frees it. The host prepared its rows
    /// for leaving the tree before.
    DropSubtree { root: NodeSlotId },
    /// Detaches the layout placement of the top layer element and clears every stale projected subtree of it.
    DetachTopLayerElement(StyleNodeID),
    /// Detaches what is left of the boxes of the node as it leaves the document, while its identity still names them:
    /// its synthetic pseudo-elements' boxes and its box's top layer placement go, and its box is cleared for the
    /// parent's rebuild to free.
    DetachRemainingRowsForRemoval(StyleNodeID),
    /// The row takes the derived style record `record` its layout node made.
    AdoptDerivedNodeStyle { node: NodeSlotId, record: u64 },
    /// The row's layout style takes the display `display`.
    SetLayoutDisplay { node: NodeSlotId, display: u32 },
    /// The anonymous rows below the row inherit its style again.
    ReinheritAnonymousDescendants { node: NodeSlotId },
    /// Prepares the row for leaving the layout tree.
    PrepareRowForDetach { row: NodeSlotId },
    /// Prepares every row of the subtree the row heads for leaving the layout tree.
    PrepareSubtreeForDetach { root: NodeSlotId },
    /// Clears the committed box of every row of the subtree the row heads, and prepares each for leaving the layout
    /// tree, as a removal does before it drops the subtree.
    PrepareSubtreeForRemoval { root: NodeSlotId },
}

/// What a write answers: whether the subtree it dropped was attached, and what it owes the host.
#[derive(Default)]
pub(crate) struct LayoutWritten {
    pub(crate) was_attached: bool,
    pub(crate) host_work: HostWorkDue,
}

impl LayoutWrite {
    /// Makes the write to the arena `arena` names, owing the host what it would have paid on its thread.
    pub(crate) fn apply(self, arena: *mut LayoutNodeArena) -> LayoutWritten {
        let host_work = OwedHostWork::default();
        let host_calls = HostCalls(&host_work);
        // SAFETY: The render state holds the arena, and nothing else reaches it while the write runs.
        let arena_ref = unsafe { &*arena };
        arena_ref.queue_box_presence();
        let was_attached = match self {
            Self::DropSubtree { root } => {
                if arena_ref.slot_is_live(root) {
                    let was_attached = arena_ref.detach_from_parent(root);
                    host_calls.free_subtree(arena, root);
                    was_attached
                } else {
                    false
                }
            }
            Self::DetachTopLayerElement(element) => {
                super::tree_builder::detach_top_layer_element_layout_subtree(host_calls, arena, element);
                false
            }
            Self::DetachRemainingRowsForRemoval(node) => {
                super::tree_builder::detach_remaining_rows_for_removal(host_calls, arena, node);
                false
            }
            Self::AdoptDerivedNodeStyle { node, record } => {
                let derived = arena_ref.with_style_engine(|engine| {
                    crate::css::style::layout_style::DerivedStyleRecord::pin(engine, record)
                });
                arena_ref.apply_reinherited_style_record(host_calls, node, derived);
                false
            }
            Self::SetLayoutDisplay { node, display } => {
                arena_ref.update_layout_style(host_calls, node, |style| {
                    style.set_display(crate::css::display::FfiDisplay::from_raw(display));
                });
                false
            }
            Self::ReinheritAnonymousDescendants { node } => {
                arena_ref.reinherit_anonymous_descendants(host_calls, node);
                false
            }
            Self::PrepareRowForDetach { row } => {
                super::layout_node_arena::prepare_row_for_detach(host_calls, arena_ref, row);
                false
            }
            Self::PrepareSubtreeForDetach { root } => {
                super::layout_node_arena::prepare_subtree_for_detach(host_calls, arena_ref, root);
                false
            }
            Self::PrepareSubtreeForRemoval { root } => {
                let mut rows = Vec::new();
                arena_ref.for_each_node_in_layout_subtree_in_pre_order(root, |row| rows.push(row));
                for &row in &rows {
                    // SAFETY: The render state holds the arena, and the clear borrows it for itself.
                    unsafe { crate::painting::ffi::paintable_cleared_from_node(host_calls, arena, row) };
                }
                for row in rows {
                    super::layout_node_arena::prepare_row_for_detach(host_calls, arena_ref, row);
                }
                false
            }
        };
        LayoutWritten {
            was_attached,
            host_work: host_work.resolve(arena_ref),
        }
    }
}

/// Has the render state of `host`'s document make `write`, spending `wait`, and answers what the write owes the host,
/// which the host pays before it goes on.
pub(crate) fn write(wait: impl RenderWait, host: &DocumentHost, write: LayoutWrite) -> LayoutWritten {
    ask(wait, host, write)
}

/// Queues `change` for the render state of `host`'s document.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on the document's thread.
pub(crate) unsafe fn queue(host: *const DocumentHost, change: LayoutChange) {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.queue_change(ArenaChange::Layout(change));
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_needs_layout_update(
    host: *const DocumentHost,
    node: NodeSlotId,
    propagate_through_ancestors: bool,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        queue(
            host,
            LayoutChange::SetNeedsLayoutUpdate {
                node,
                propagate_through_ancestors,
            },
        );
    }
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_needs_full_layout_tree_update(host: *const DocumentHost, value: bool) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::SetNeedsFullLayoutTreeUpdate(value)) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_reset_cached_intrinsic_sizes_of_self_and_ancestors(
    host: *const DocumentHost,
    node: NodeSlotId,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::ResetCachedIntrinsicSizesOfSelfAndAncestors { node }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_defer_child_list_insertion_layout_update(
    host: *const DocumentHost,
    parent: NodeSlotId,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::DeferChildListInsertionLayoutUpdate { parent }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_invalidate_text_content(host: *const DocumentHost, node: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::InvalidateTextContent { node }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_enroll_text_after_language_change(host: *const DocumentHost, root: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::EnrollTextAfterLanguageChange { root }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_record_partial_relayout_escape(host: *const DocumentHost) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::RecordPartialRelayoutEscape) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_note_contained_abspos_child_removal(
    host: *const DocumentHost,
    parent: NodeSlotId,
    child: NodeSlotId,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::NoteContainedAbsposChildRemoval { parent, child }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread, and `generated_for` must point at `count` readable
/// pseudo-element kinds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_style_node_changed(
    host: *const DocumentHost,
    old: u32,
    new: u32,
    generated_for: *const u8,
    count: usize,
) {
    let generated_for = if count == 0 {
        SmallVec::new()
    } else {
        // SAFETY: Guaranteed by the caller.
        SmallVec::from_slice(unsafe { std::slice::from_raw_parts(generated_for, count) })
    };
    let change = LayoutChange::StyleNodeChanged {
        old: StyleNodeID::from_raw(old),
        new: StyleNodeID::from_raw(new),
        generated_for,
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread, and `elements` must point at `count` readable
/// element identities.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_anchor_name_elements(
    host: *const DocumentHost,
    scope_host: u32,
    anchor_name: usize,
    elements: *const u32,
    count: usize,
) {
    let elements: &[u32] = if count == 0 {
        &[]
    } else {
        // SAFETY: Guaranteed by the caller.
        unsafe { std::slice::from_raw_parts(elements, count) }
    };
    let change = LayoutChange::SetAnchorNameElements {
        scope_host: StyleNodeID::from_raw(scope_host),
        anchor_name,
        elements: elements.iter().copied().filter_map(StyleNodeID::from_raw).collect(),
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_element_scroll_offset(
    host: *const DocumentHost,
    element: u32,
    offset: FfiCssPixelPoint,
) {
    let Some(element) = StyleNodeID::from_raw(element) else {
        return;
    };
    let offset = offset.into();
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::SetElementScrollOffset { element, offset }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_pseudo_element_scroll_offset(
    host: *const DocumentHost,
    generator: u32,
    generated_for: u8,
    offset: FfiCssPixelPoint,
) {
    let Some(generator) = StyleNodeID::from_raw(generator) else {
        return;
    };
    let change = LayoutChange::SetPseudoElementScrollOffset {
        generator,
        generated_for,
        offset: offset.into(),
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_identity_in_focused_text_control(
    host: *const DocumentHost,
    node: u32,
    value: bool,
) {
    let Some(node) = StyleNodeID::from_raw(node) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::SetIdentityInFocusedTextControl { node, value }) };
}

/// Publishes what the SVG element `element` names parses to. A `<polyline>` or `<polygon>` passes its `points` list
/// beside the facts, since it is the one geometry attribute that is not a fixed number of values.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread, and `points` must point at `count` readable points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_svg_attribute_facts(
    host: *const DocumentHost,
    element: u32,
    facts: FfiSvgAttributeFacts,
    points: *const FfiFloatPoint,
    count: usize,
) {
    let Some(element) = StyleNodeID::from_raw(element) else {
        return;
    };
    let points = if count == 0 {
        &[][..]
    } else {
        // SAFETY: Guaranteed by the caller.
        unsafe { std::slice::from_raw_parts(points, count) }
    };
    let change = LayoutChange::SvgAttributeFacts {
        element,
        facts: Box::new(facts),
        points: points.into(),
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}

/// Publishes only the resources a graphics element's style names, leaving what its attributes parse to alone. Style
/// records are replaced far more often than an SVG attribute changes, and parsing every presentation attribute again
/// to carry four names would make every style change pay for it.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_svg_style_references(
    host: *const DocumentHost,
    element: u32,
    mask: u32,
    clip_path: u32,
    fill: u32,
    stroke: u32,
) {
    let Some(element) = StyleNodeID::from_raw(element) else {
        return;
    };
    let change = LayoutChange::SvgStyleReferences {
        element,
        references: [mask, clip_path, fill, stroke],
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_document_is_decoded_svg(host: *const DocumentHost, is_decoded_svg: bool) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::SetDocumentIsDecodedSvg(is_decoded_svg)) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_node_flag(
    host: *const DocumentHost,
    node: NodeSlotId,
    flag: NodeFlag,
    value: bool,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::SetNodeFlag { node, flag, value }) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_owned_image_natural_size(
    host: *const DocumentHost,
    node: NodeSlotId,
    natural_size: FfiNaturalSize,
) {
    let change = LayoutChange::SetOwnedImageNaturalSize {
        node,
        natural_size: natural_size.into(),
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_invalidate_searchable_text(host: *const DocumentHost) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::InvalidateSearchableText) };
}

/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_restamp_table_spans(host: *const DocumentHost, node: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::RestampTableSpans { node }) };
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeKind;

    #[test]
    fn a_change_to_a_freed_node_changes_nothing() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test();
        arena.write_shape(node.slot).set_kind(NodeKind::BlockContainer);
        let freed = node.slot;
        arena
            .free_subtree(freed)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        LayoutChange::SetNeedsLayoutUpdate {
            node: freed,
            propagate_through_ancestors: true,
        }
        .apply(&mut arena);
        assert!(!arena.slot_is_live(freed));
    }

    #[test]
    fn a_layout_mark_reaches_the_arena() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.write_shape(node).set_kind(NodeKind::BlockContainer);
        arena.reset_layout_update_flags_in_subtree(node);
        assert!(!arena.node_needs_layout_update(node));
        LayoutChange::SetNeedsLayoutUpdate {
            node,
            propagate_through_ancestors: false,
        }
        .apply(&mut arena);
        assert!(arena.node_needs_layout_update(node));
        arena
            .free_subtree(node)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn a_dropped_subtree_owes_the_host_its_freed_rows_until_it_is_paid() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        arena.insert_child(parent, child, NodeSlotId::INVALID);
        let written = LayoutWrite::DropSubtree { root: child }.apply(&raw mut arena);
        assert!(written.was_attached);
        assert!(!arena.slot_is_live(child));
        written.host_work.pay(&crate::stage::MainThread::for_test());
        let written = LayoutWrite::DropSubtree { root: parent }.apply(&raw mut arena);
        assert!(!written.was_attached);
        written.host_work.pay(&crate::stage::MainThread::for_test());
        assert_eq!(arena.live_slot_count(), 0);
    }
}
