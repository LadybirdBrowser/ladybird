/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

use crate::abort_on_panic;
use crate::layout::layout_node_arena::{FfiAnonymousStyleKind, FfiAnonymousStyleOverrides, LayoutNodeArena};
use crate::layout::node_data::{GENERATED_FOR_AFTER, GENERATED_FOR_MARKER, NodeData, NodeFlag, NodeKind, NodeSlotId};
use crate::layout::text_chunker::{GraphemeSegmenter, code_point_at, code_unit_length_for_code_point};
use crate::layout::tree_mutation::{UnplacedLayoutNode, free_subtree_and_destroy_shells};
use crate::layout::{ComputedValuesView, FfiDisplay};
use std::ffi::c_void;

type LayoutNode = NodeSlotId;

pub(crate) struct TreeBuilderState {
    pub(crate) ancestor_stack: Vec<LayoutNode>,
    pub(crate) quote_nesting_level: u32,
    // Partial-rebuild bookkeeping: boxes replaced in place become rebuild roots, and any tree
    // restructuring that reaches outside every rebuild root downgrades the update to a full one.
    current_rebuild_root: LayoutNode,
    rebuilt_subtree_root_shells: Vec<*mut c_void>,
    rebuilt_subtree_roots: Vec<LayoutNode>,
    reused_child_list_update_roots: Vec<LayoutNode>,
    additional_table_fixup_roots: Vec<LayoutNode>,
    layout_tree_update_escaped_rebuild_roots: bool,
    new_subtree_root: LayoutNode,
    layout_tree_rebuild_requests: Vec<*mut c_void>,
}

impl Default for TreeBuilderState {
    fn default() -> Self {
        Self {
            ancestor_stack: Vec::new(),
            quote_nesting_level: 0,
            current_rebuild_root: NodeSlotId::INVALID,
            rebuilt_subtree_root_shells: Vec::new(),
            rebuilt_subtree_roots: Vec::new(),
            reused_child_list_update_roots: Vec::new(),
            additional_table_fixup_roots: Vec::new(),
            layout_tree_update_escaped_rebuild_roots: false,
            new_subtree_root: NodeSlotId::INVALID,
            layout_tree_rebuild_requests: Vec::new(),
        }
    }
}

#[derive(Default)]
pub(crate) struct TreeBuilderContext {
    pub(crate) has_svg_root: bool,
    pub(crate) layout_top_layer: bool,
    layout_svg_mask_or_clip_path: bool,
    layout_svg_pattern: bool,
}

impl TreeBuilderState {
    pub(crate) fn current_parent(&self) -> LayoutNode {
        *self
            .ancestor_stack
            .last()
            .expect("layout tree builder must have an insertion ancestor")
    }
}

// How clear_stale_subtree walks the shadow-including subtree. Bounded scopes clear SVG resource
// boxes whose layout attachment lies inside the cleared root; the unbounded scope always lets
// them survive the cleanup.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiStaleSubtreeClearScope {
    Inclusive,
    InclusiveBoundedToRoot,
    DescendantsBoundedToRoot,
}

#[repr(C)]
pub struct FfiDomTreeBuilderCallbacks {
    pub builder: *mut c_void,
    pub first_child: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub next_sibling: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub clear_dom_update_flags: unsafe extern "C" fn(*mut c_void),
    pub assigned_node_count: unsafe extern "C" fn(*mut c_void) -> usize,
    pub assigned_node_at: unsafe extern "C" fn(*mut c_void, usize) -> *mut c_void,
    pub is_svg_element: unsafe extern "C" fn(*mut c_void) -> bool,
    pub clear_stale_layout_node: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub display_contents_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiDisplayContentsFacts,
    pub clear_stale_subtree: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiStaleSubtreeClearScope),
    pub resolve_counters: unsafe extern "C" fn(*mut c_void, FfiPseudoElement),
    pub principal_descendant_facts:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> FfiPrincipalDescendantFacts,
    pub layout_node_has_first_letter_style: unsafe extern "C" fn(*mut c_void) -> bool,
    pub create_first_letter_nodes:
        unsafe extern "C" fn(*mut c_void, *mut c_void, FfiFirstLetterTarget) -> FfiFirstLetterNodes,
    pub top_layer_element_count: unsafe extern "C" fn(*mut c_void) -> usize,
    pub copy_top_layer_elements: unsafe extern "C" fn(*mut c_void, *mut *mut c_void, usize),
    pub rendered_in_top_layer: unsafe extern "C" fn(*mut c_void) -> bool,
    pub flat_tree_parent: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub flat_tree_render_facts: unsafe extern "C" fn(*mut c_void) -> FfiFlatTreeRenderFacts,
    pub svg_pattern_content_element: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub register_svg_resource_reference: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub element_layout_node: unsafe extern "C" fn(*mut c_void) -> NodeSlotId,
    pub dom_node_layout_node: unsafe extern "C" fn(*mut c_void) -> NodeSlotId,
    pub layout_node_dom_element: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub element_pseudo_layout_node: unsafe extern "C" fn(*mut c_void, FfiPseudoElement) -> NodeSlotId,
    pub principal_node_entry_facts: unsafe extern "C" fn(*mut c_void, *mut c_void, bool) -> FfiPrincipalNodeEntryFacts,
    pub request_top_layer_zone_rebuild: unsafe extern "C" fn(*mut c_void),
    pub request_layout_tree_rebuild: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub push_principal_frame: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiPrincipalNodeFrame,
    pub pop_principal_frame: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub prepare_principal_element:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, bool) -> FfiPreparedPrincipalElementFacts,
    pub principal_element_layout_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiElementLayoutFacts,
    pub create_principal_element_layout:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, FfiElementLayoutKind) -> NodeSlotId,
    pub create_principal_document_layout: unsafe extern "C" fn(*mut c_void, *mut c_void) -> NodeSlotId,
    pub principal_text_layout_facts: unsafe extern "C" fn(*mut c_void) -> FfiTextLayoutFacts,
    pub create_principal_text_layout: unsafe extern "C" fn(*mut c_void, *mut c_void) -> NodeSlotId,
    pub set_principal_layout_node: unsafe extern "C" fn(*mut c_void, *mut c_void, NodeSlotId),
    pub reuse_principal_layout: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub principal_layout_node: unsafe extern "C" fn(*mut c_void) -> NodeSlotId,
    pub attach_principal_style_resources: unsafe extern "C" fn(*mut c_void),
    pub apply_replaced_display_adjustment: unsafe extern "C" fn(*mut c_void, FfiReplacedElementDisplayAdjustment),
    pub set_layout_root: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub document_layout_node: unsafe extern "C" fn(*mut c_void) -> NodeSlotId,
    pub report_rebuild_outcome: unsafe extern "C" fn(*mut c_void, *const *mut c_void, usize, bool),
    pub layout: FfiTreeBuilderCallbacks,
    pub pseudo: FfiPseudoTreeBuilderCallbacks,
}

/// The C++ frame that retains a principal node's old and new layout boxes, paired with the old
/// box's arena slot so Rust can reason about in-place replacement without calling back.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPrincipalNodeFrame {
    pub frame: *mut c_void,
    pub old_layout_node: NodeSlotId,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPreparedPrincipalElementFacts {
    pub display: FfiPrincipalDisplayFacts,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiDisplayContentsFacts {
    pub rendered_in_top_layer: bool,
    pub content_visibility_hidden: bool,
    pub should_layout_dom_children: bool,
    pub child_needs_layout_tree_update: bool,
    pub dom_children_parent: *mut c_void,
    pub shadow_root: *mut c_void,
    pub slot_element: *mut c_void,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiTextLayoutFacts {
    pub has_style_parent: bool,
    pub parent_display_is_contents: bool,
    pub text_is_ascii_whitespace: bool,
    pub parent_collapses_whitespace: bool,
    pub style_parent_style_record: u64,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiFlatTreeRenderFacts {
    pub is_element: bool,
    pub has_computed_style: bool,
    pub display_is_none: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPrincipalDescendantFacts {
    pub is_element: bool,
    pub content_visibility_hidden: bool,
    pub should_layout_dom_children: bool,
    pub child_needs_layout_tree_update: bool,
    pub is_svg_switch_element: bool,
    pub is_document: bool,
    pub dom_children_parent: *mut c_void,
    pub shadow_root: *mut c_void,
    pub slot_element: *mut c_void,
    pub svg_graphics_element: *mut c_void,
    pub svg_mask: *mut c_void,
    pub svg_clip_path: *mut c_void,
    pub svg_fill_pattern: *mut c_void,
    pub svg_stroke_pattern: *mut c_void,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPrincipalNodeEntryFacts {
    pub must_create_subtree: bool,
    pub needs_layout_tree_update: bool,
    pub may_reuse_layout_node_for_child_list_insertion: bool,
    pub document_needs_full_layout_tree_update: bool,
    pub is_document: bool,
    pub has_layout_node: bool,
    pub is_element: bool,
    pub is_text: bool,
    pub rendered_in_top_layer: bool,
    pub layout_node_is_attached: bool,
    pub is_svg_container: bool,
    pub requires_svg_container: bool,
    pub is_svg_foreign_object: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPrincipalDisplayFacts {
    pub display_is_none: bool,
    pub display_is_contents: bool,
    pub display_is_table_inside: bool,
    pub display_is_block_outside: bool,
    pub display_is_internal_table: bool,
    pub display_is_table_caption: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiElementLayoutFacts {
    pub has_content_replacement: bool,
    pub is_svg_mask_element: bool,
    pub is_svg_clip_path_element: bool,
    pub is_svg_pattern_element: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiElementLayoutKind {
    ContentReplacement,
    SvgMask,
    SvgClipPath,
    SvgPattern,
    Normal,
}

pub(crate) fn element_layout_kind(
    facts: FfiElementLayoutFacts,
    layout_svg_mask_or_clip_path: bool,
    layout_svg_pattern: bool,
) -> FfiElementLayoutKind {
    if facts.has_content_replacement {
        FfiElementLayoutKind::ContentReplacement
    } else if layout_svg_mask_or_clip_path {
        if facts.is_svg_mask_element {
            FfiElementLayoutKind::SvgMask
        } else {
            assert!(facts.is_svg_clip_path_element);
            FfiElementLayoutKind::SvgClipPath
        }
    } else if layout_svg_pattern {
        assert!(facts.is_svg_pattern_element);
        FfiElementLayoutKind::SvgPattern
    } else {
        FfiElementLayoutKind::Normal
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum TopLayerEntryDecision {
    Continue,
    Skip,
    SkipAndRequestZoneRebuild,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SvgEntryDecision {
    Continue,
    EnterSvgRoot,
    EnterForeignContent,
    Skip,
}

#[derive(Clone, Copy)]
pub(crate) struct PrincipalNodeEntryDecision {
    pub(crate) should_create_layout_node: bool,
    pub(crate) top_layer: TopLayerEntryDecision,
    pub(crate) svg: SvgEntryDecision,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PrincipalBoxGenerationDecision {
    Suppress,
    DisplayContents,
    PrincipalBox,
}

pub(crate) fn principal_box_generation_decision(
    is_element: bool,
    display_is_none: bool,
    display_is_contents: bool,
) -> PrincipalBoxGenerationDecision {
    abort_on_panic(|| {
        if is_element && display_is_none {
            PrincipalBoxGenerationDecision::Suppress
        } else if is_element && display_is_contents {
            PrincipalBoxGenerationDecision::DisplayContents
        } else {
            PrincipalBoxGenerationDecision::PrincipalBox
        }
    })
}

pub(crate) fn display_contents_text_needs_style_wrapper(
    has_style_parent: bool,
    parent_display_is_contents: bool,
    text_is_ascii_whitespace: bool,
    parent_collapses_whitespace: bool,
) -> bool {
    abort_on_panic(|| {
        has_style_parent && parent_display_is_contents && (!text_is_ascii_whitespace || !parent_collapses_whitespace)
    })
}

#[repr(C)]
pub struct FfiStaleNodeCallbacks {
    pub layout_dom_node: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub dom_is_shadow_including_inclusive_descendant: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
}

/// Returns whether an SVG resource layout node must survive cleanup of a DOM subtree.
///
/// # Safety
///
/// The callback table, arena, layout node, and optional cleared subtree root must remain valid for the duration of the
/// call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_should_preserve_svg_resource_layout_node(
    callbacks: *const FfiStaleNodeCallbacks,
    arena: *mut c_void,
    layout_node: NodeSlotId,
    cleared_subtree_root: *mut c_void,
) -> bool {
    assert!(!callbacks.is_null());
    assert!(!arena.is_null());
    assert!(!layout_node.is_invalid());
    // SAFETY: Guaranteed by the entry point's contract.
    let callbacks = unsafe { &*callbacks };
    let arena = arena.cast::<LayoutNodeArena>();
    if cleared_subtree_root.is_null() {
        return true;
    }

    // SAFETY: The arena and layout node remain live throughout the ancestor walk.
    let mut ancestor = unsafe { &*arena }.data(layout_node).parent.get();
    while !ancestor.is_invalid() {
        // SAFETY: `ancestor` is a live layout node, and a non-anonymous one has a shell.
        let data = unsafe { &*arena }.data(ancestor);
        let parent = data.parent.get();
        if data.flags.get() & NodeFlag::Anonymous as u32 != 0 {
            ancestor = parent;
            continue;
        }
        let dom_node = unsafe { (callbacks.layout_dom_node)(data.shell.get()) };
        // SAFETY: Both DOM pointers remain live throughout cleanup.
        if !dom_node.is_null()
            && unsafe { (callbacks.dom_is_shadow_including_inclusive_descendant)(dom_node, cleared_subtree_root) }
        {
            return false;
        }
        ancestor = parent;
    }
    true
}

#[repr(C)]
pub struct FfiTopLayerDetachCallbacks {
    pub element_layout_node: unsafe extern "C" fn(*mut c_void) -> NodeSlotId,
    pub prepare_subtree_for_detach: unsafe extern "C" fn(*mut c_void),
    pub clear_stale_subtree: unsafe extern "C" fn(*mut c_void),
    pub slot_element: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub assigned_node_count: unsafe extern "C" fn(*mut c_void) -> usize,
    pub assigned_node_at: unsafe extern "C" fn(*mut c_void, usize) -> *mut c_void,
}

/// Finds the box to detach for a top-layer element: the element's own box, or the outermost
/// anonymous wrapper around it that is a direct viewport child. Leaving an empty anonymous
/// table-fixup wrapper as a viewport child would violate layout invariants.
fn topmost_layout_node_of_top_layer_placement(arena: *mut LayoutNodeArena, layout_node: NodeSlotId) -> NodeSlotId {
    let mut direct_viewport_child_candidate = layout_node;
    loop {
        // SAFETY: The caller guarantees a live arena, and parent links only name live slots.
        let parent = unsafe { &*arena }.data(direct_viewport_child_candidate).parent.get();
        if parent.is_invalid() {
            return NodeSlotId::INVALID;
        }
        // SAFETY: `parent` is a live layout node.
        let parent_data = unsafe { &*arena }.data(parent);
        if !node_has_flag(parent_data, NodeFlag::Anonymous) {
            return if parent_data.kind.get() == NodeKind::Viewport {
                direct_viewport_child_candidate
            } else {
                NodeSlotId::INVALID
            };
        }
        direct_viewport_child_candidate = parent;
    }
}

/// Detaches a top-layer element's layout placement and clears every stale projected subtree.
///
/// # Safety
///
/// The callback table, arena, and element must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_detach_top_layer_element_layout_subtree(
    callbacks: *const FfiTopLayerDetachCallbacks,
    arena: *mut c_void,
    element: *mut c_void,
) {
    assert!(!callbacks.is_null());
    assert!(!arena.is_null());
    assert!(!element.is_null());
    // SAFETY: Guaranteed by the entry point's contract.
    let callbacks = unsafe { &*callbacks };
    let arena = arena.cast::<LayoutNodeArena>();
    // SAFETY: The element remains live throughout the call.
    let element_layout_node = unsafe { (callbacks.element_layout_node)(element) };
    if !element_layout_node.is_invalid() {
        let topmost = topmost_layout_node_of_top_layer_placement(arena, element_layout_node);
        let layout_node_to_detach = if topmost.is_invalid() {
            element_layout_node
        } else {
            topmost
        };
        let shell = unsafe { &*arena }.node_shell(layout_node_to_detach);
        // SAFETY: The C++ detach preparation walks the still-linked subtree; the shared arena
        // borrow ends before the subtree is freed.
        unsafe { (callbacks.prepare_subtree_for_detach)(shell) };
        if unsafe { &*arena }.detach_from_parent(layout_node_to_detach) {
            free_subtree_and_destroy_shells(arena, layout_node_to_detach);
        }
    }

    // SAFETY: The element remains live throughout subtree cleanup.
    unsafe { (callbacks.clear_stale_subtree)(element) };
    // SAFETY: The callback returns the element's adjusted HTMLSlotElement pointer, if any.
    let slot_element = unsafe { (callbacks.slot_element)(element) };
    if !slot_element.is_null() {
        clear_stale_assigned_slottables(
            slot_element,
            |slot| unsafe { (callbacks.assigned_node_count)(slot) },
            |slot, index| unsafe { (callbacks.assigned_node_at)(slot, index) },
            |root| unsafe { (callbacks.clear_stale_subtree)(root) },
        );
    }
}

fn clear_stale_assigned_slottables(
    slot_element: *mut c_void,
    assigned_node_count: impl Fn(*mut c_void) -> usize,
    assigned_node_at: impl Fn(*mut c_void, usize) -> *mut c_void,
    clear_assigned_subtree: impl Fn(*mut c_void),
) {
    assert!(!slot_element.is_null());
    let count = assigned_node_count(slot_element);
    for index in 0..count {
        let root = assigned_node_at(slot_element, index);
        assert!(!root.is_null());
        clear_assigned_subtree(root);
    }
}

#[derive(Clone, Copy)]
pub(crate) struct PrincipalBoxPlacementFacts {
    pub(crate) must_create_subtree: bool,
    pub(crate) should_create_layout_node: bool,
    pub(crate) has_old_layout_node: bool,
    pub(crate) old_layout_node_is_attached: bool,
    pub(crate) old_and_new_layout_nodes_are_same: bool,
    pub(crate) has_current_rebuild_root: bool,
    pub(crate) is_in_dom_order_insertion: bool,
    pub(crate) is_document: bool,
    pub(crate) is_element: bool,
    pub(crate) rendered_in_top_layer: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FfiPrincipalBoxPlacement {
    None,
    DocumentRoot,
    ReplaceExisting,
    AppendSvg,
    NormalInsertion,
}

#[derive(Clone, Copy)]
pub(crate) struct PrincipalBoxPlacementDecision {
    pub(crate) placement: FfiPrincipalBoxPlacement,
    may_replace_existing_layout_node: bool,
    pub(crate) start_rebuild_root: bool,
    pub(crate) mark_update_escaped_rebuild_roots: bool,
    pub(crate) create_backdrop: bool,
    pub(crate) clear_layout_top_layer_for_descendants: bool,
}

pub(crate) fn principal_box_placement_decision(
    facts: PrincipalBoxPlacementFacts,
    layout_node_is_svg_box: bool,
    layout_top_layer: bool,
) -> PrincipalBoxPlacementDecision {
    abort_on_panic(|| {
        let may_replace_existing_layout_node = !facts.must_create_subtree
            && facts.has_old_layout_node
            && facts.old_layout_node_is_attached
            && !facts.old_and_new_layout_nodes_are_same;
        let start_rebuild_root = (may_replace_existing_layout_node
            || (facts.should_create_layout_node && !facts.has_old_layout_node && facts.is_in_dom_order_insertion))
            && !facts.has_current_rebuild_root;
        let mark_update_escaped_rebuild_roots = facts.should_create_layout_node
            && !facts.has_old_layout_node
            && !facts.has_current_rebuild_root
            && !facts.is_in_dom_order_insertion
            && !facts.is_document;

        let placement = if facts.is_document {
            FfiPrincipalBoxPlacement::DocumentRoot
        } else if !facts.should_create_layout_node {
            FfiPrincipalBoxPlacement::None
        } else if may_replace_existing_layout_node {
            FfiPrincipalBoxPlacement::ReplaceExisting
        } else if layout_node_is_svg_box {
            FfiPrincipalBoxPlacement::AppendSvg
        } else {
            FfiPrincipalBoxPlacement::NormalInsertion
        };

        let is_active_top_layer_member = facts.is_element && facts.rendered_in_top_layer && layout_top_layer;
        PrincipalBoxPlacementDecision {
            placement,
            may_replace_existing_layout_node,
            start_rebuild_root,
            mark_update_escaped_rebuild_roots,
            create_backdrop: facts.should_create_layout_node && is_active_top_layer_member,
            clear_layout_top_layer_for_descendants: is_active_top_layer_member,
        }
    })
}

pub(crate) fn principal_node_entry_decision(
    facts: FfiPrincipalNodeEntryFacts,
    context: &TreeBuilderContext,
) -> PrincipalNodeEntryDecision {
    abort_on_panic(|| {
        let should_create_layout_node = facts.must_create_subtree
            || (facts.needs_layout_tree_update && !facts.may_reuse_layout_node_for_child_list_insertion)
            || facts.document_needs_full_layout_tree_update
            || (facts.is_document && !facts.has_layout_node);

        let top_layer = if facts.is_element && facts.rendered_in_top_layer && !context.layout_top_layer {
            if !facts.layout_node_is_attached && !facts.needs_layout_tree_update {
                TopLayerEntryDecision::SkipAndRequestZoneRebuild
            } else {
                TopLayerEntryDecision::Skip
            }
        } else {
            TopLayerEntryDecision::Continue
        };

        let svg = if facts.is_svg_container {
            SvgEntryDecision::EnterSvgRoot
        } else if facts.requires_svg_container && !context.has_svg_root {
            SvgEntryDecision::Skip
        } else if facts.is_svg_foreign_object {
            SvgEntryDecision::EnterForeignContent
        } else if facts.is_element && !facts.requires_svg_container && context.has_svg_root {
            SvgEntryDecision::Skip
        } else {
            SvgEntryDecision::Continue
        };

        PrincipalNodeEntryDecision {
            should_create_layout_node,
            top_layer,
            svg,
        }
    })
}

struct DomTreeBuilderHost<'a> {
    callbacks: &'a FfiDomTreeBuilderCallbacks,
    arena: *mut LayoutNodeArena,
}

impl DomTreeBuilderHost<'_> {
    fn first_child(&self, parent: *mut c_void) -> *mut c_void {
        // SAFETY: Entry points guarantee that `parent` is a live ParentNode.
        unsafe { (self.callbacks.first_child)(parent) }
    }

    fn next_sibling(&self, node: *mut c_void) -> *mut c_void {
        // SAFETY: Callers only pass live DOM nodes.
        unsafe { (self.callbacks.next_sibling)(node) }
    }

    fn dom_node_layout_node(&self, node: *mut c_void) -> LayoutNode {
        // SAFETY: Callers only pass live DOM nodes.
        unsafe { (self.callbacks.dom_node_layout_node)(node) }
    }

    fn layout(&self) -> TreeBuilderHost<'_> {
        TreeBuilderHost {
            callbacks: &self.callbacks.layout,
            arena: self.arena,
        }
    }
}

fn has_unrendered_flat_tree_ancestor(host: &DomTreeBuilderHost<'_>, element: *mut c_void) -> bool {
    // SAFETY: `element` and every returned flat-tree ancestor remain live throughout layout-tree construction.
    let mut ancestor = unsafe { (host.callbacks.flat_tree_parent)(element) };
    while !ancestor.is_null() {
        // SAFETY: `ancestor` is a live DOM node.
        let facts = unsafe { (host.callbacks.flat_tree_render_facts)(ancestor) };
        // Null style means the style update pass skipped a display:none subtree.
        if facts.is_element && (!facts.has_computed_style || facts.display_is_none) {
            return true;
        }
        // SAFETY: `ancestor` remains live throughout the walk.
        ancestor = unsafe { (host.callbacks.flat_tree_parent)(ancestor) };
    }
    false
}

unsafe fn dom_tree_builder_host<'a>(
    callbacks: *const FfiDomTreeBuilderCallbacks,
    arena: *mut c_void,
) -> DomTreeBuilderHost<'a> {
    assert!(!callbacks.is_null());
    assert!(!arena.is_null());
    // SAFETY: Each exported entry point requires the callback table to remain live for the duration of its call.
    DomTreeBuilderHost {
        callbacks: unsafe { &*callbacks },
        arena: arena.cast(),
    }
}

/// Updates every direct DOM child in tree order.
///
/// # Safety
///
/// The callback table, parent, and context must remain valid for the duration of the call.
unsafe fn update_layout_tree_for_dom_children(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    parent: *mut c_void,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
    insertion_mode: FfiInsertionMode,
) {
    abort_on_panic(|| {
        assert!(!parent.is_null());
        let mut node = host.first_child(parent);
        while !node.is_null() {
            update_layout_tree(host, state, node, context, must_create_subtree, insertion_mode);
            node = host.next_sibling(node);
        }
    });
}

/// Updates every shadow-root child in tree order and clears the root's update flags.
///
/// # Safety
///
/// The callback table, shadow root, and context must remain valid for the duration of the call.
unsafe fn update_layout_tree_for_shadow_root_children(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    shadow_root: *mut c_void,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
) {
    abort_on_panic(|| {
        assert!(!shadow_root.is_null());
        let mut node = host.first_child(shadow_root);
        while !node.is_null() {
            update_layout_tree(
                host,
                state,
                node,
                context,
                must_create_subtree,
                FfiInsertionMode::Append,
            );
            node = host.next_sibling(node);
        }
        // SAFETY: `shadow_root` remains live throughout the call.
        unsafe { (host.callbacks.clear_dom_update_flags)(shadow_root) };
    });
}

/// Updates a slot's assigned nodes in flat-tree order.
///
/// # Safety
///
/// The callback table, slot element, and context must remain valid for the duration of the call.
unsafe fn update_layout_tree_for_assigned_slottables(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    slot_element: *mut c_void,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
) {
    abort_on_panic(|| {
        assert!(!slot_element.is_null());
        // SAFETY: `slot_element` remains live throughout the call.
        let assigned_node_count = unsafe { (host.callbacks.assigned_node_count)(slot_element) };
        for index in 0..assigned_node_count {
            // SAFETY: `index` is below the count reported for this unchanged assigned-node list.
            let node = unsafe { (host.callbacks.assigned_node_at)(slot_element, index) };
            assert!(!node.is_null());
            update_layout_tree(
                host,
                state,
                node,
                context,
                must_create_subtree,
                FfiInsertionMode::Append,
            );
        }
    });
}

/// Applies SVG `<switch>` child selection and updates its rendered child.
///
/// # Safety
///
/// The callback table, switch element, and context must remain valid for the duration of the call.
unsafe fn update_layout_tree_for_svg_switch_children(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    switch_element: *mut c_void,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
) {
    abort_on_panic(|| {
        assert!(!switch_element.is_null());

        // https://svgwg.org/svg2-draft/struct.html#SwitchElement
        // The ‘switch’ element evaluates the ‘requiredExtensions’ and ‘systemLanguage’ attributes on its direct child
        // elements in order, and then processes and renders the first child for which these attributes evaluate to
        // true. All others will be bypassed and therefore not rendered. If the child element is a container element
        // such as a ‘g’, then the entire subtree is either processed/rendered or bypassed/not rendered.
        let mut rendered_child = std::ptr::null_mut();
        let mut child = host.first_child(switch_element);
        while !child.is_null() {
            // FIXME: Evaluate the requiredExtensions and systemLanguage attributes.
            // SAFETY: `child` is a live DOM node.
            if unsafe { (host.callbacks.is_svg_element)(child) } {
                rendered_child = child;
                break;
            }
            child = host.next_sibling(child);
        }

        // NB: Clean up any stale children that should no longer be rendered.
        let mut child = host.first_child(switch_element);
        while !child.is_null() {
            if child != rendered_child {
                // SAFETY: The builder and `child` remain live throughout the call.
                unsafe {
                    (host.callbacks.clear_stale_layout_node)(host.callbacks.builder, child);
                }
            }
            child = host.next_sibling(child);
        }

        if !rendered_child.is_null() {
            update_layout_tree(
                host,
                state,
                rendered_child,
                context,
                must_create_subtree,
                FfiInsertionMode::Append,
            );
        }
    });
}

/// Updates an element that generates no principal box because it has `display: contents`.
///
/// # Safety
///
/// The callback table, element, and context must remain valid for the duration of the call.
unsafe fn update_layout_tree_for_display_contents(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    element: *mut c_void,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
    should_create_layout_node: bool,
) {
    abort_on_panic(|| {
        assert!(!element.is_null());
        // SAFETY: The element remains live for the duration of the call.
        let facts = unsafe { (host.callbacks.display_contents_facts)(host.callbacks.builder, element) };

        // A display:contents member builds its children through this path, so the top layer flag
        // is consumed here the same way update_layout_tree does for members with a box.
        let clear_layout_top_layer_for_descendants = facts.rendered_in_top_layer && context.layout_top_layer;
        if clear_layout_top_layer_for_descendants {
            context.layout_top_layer = false;
        }

        if should_create_layout_node {
            // SAFETY: The builder and element remain live throughout this call.
            unsafe {
                (host.callbacks.clear_stale_subtree)(
                    host.callbacks.builder,
                    element,
                    FfiStaleSubtreeClearScope::Inclusive,
                );
                (host.callbacks.resolve_counters)(element, FfiPseudoElement::None);
            }
        }

        if should_create_layout_node && !facts.content_visibility_hidden && !context.has_svg_root {
            let placed = create_pseudo_element(
                host,
                state,
                element,
                FfiPseudoElement::Before,
                Some(FfiInsertionMode::Append),
            );
            assert!(placed.is_none());
        }

        if !facts.content_visibility_hidden && (should_create_layout_node || facts.child_needs_layout_tree_update) {
            let must_create_children = should_create_layout_node;
            if !facts.shadow_root.is_null() {
                // SAFETY: The callback table, shadow root, and context remain valid.
                unsafe {
                    update_layout_tree_for_shadow_root_children(
                        host,
                        state,
                        facts.shadow_root,
                        context,
                        must_create_children,
                    );
                }
            } else if facts.should_layout_dom_children {
                assert!(!facts.dom_children_parent.is_null());
                // SAFETY: The callback table, parent, and context remain valid.
                unsafe {
                    update_layout_tree_for_dom_children(
                        host,
                        state,
                        facts.dom_children_parent,
                        context,
                        must_create_children,
                        FfiInsertionMode::Append,
                    );
                }
            }
        }

        if !facts.slot_element.is_null() {
            if !facts.content_visibility_hidden {
                // SAFETY: The callback table, slot element, and context remain valid.
                unsafe {
                    update_layout_tree_for_assigned_slottables(
                        host,
                        state,
                        facts.slot_element,
                        context,
                        must_create_subtree || should_create_layout_node,
                    );
                }
            } else {
                clear_stale_assigned_slottables(
                    facts.slot_element,
                    |slot| unsafe { (host.callbacks.assigned_node_count)(slot) },
                    |slot, index| unsafe { (host.callbacks.assigned_node_at)(slot, index) },
                    |root| unsafe {
                        (host.callbacks.clear_stale_subtree)(
                            host.callbacks.builder,
                            root,
                            FfiStaleSubtreeClearScope::InclusiveBoundedToRoot,
                        );
                    },
                );
            }
        }

        if should_create_layout_node && !facts.content_visibility_hidden && !context.has_svg_root {
            let placed = create_pseudo_element(
                host,
                state,
                element,
                FfiPseudoElement::After,
                Some(FfiInsertionMode::Append),
            );
            assert!(placed.is_none());
        }

        assert!(!facts.dom_children_parent.is_null());
        // SAFETY: The element's ParentNode subobject remains live throughout this call.
        unsafe { (host.callbacks.clear_dom_update_flags)(facts.dom_children_parent) };

        if clear_layout_top_layer_for_descendants {
            context.layout_top_layer = true;
        }
    });
}

fn ancestor_stack_contains_element_layout_node(
    host: &DomTreeBuilderHost<'_>,
    state: &TreeBuilderState,
    element: *mut c_void,
) -> bool {
    // An element's box registers itself on the element at construction time, before its subtree
    // is built, so an element under construction anywhere on the ancestor stack is found here.
    // SAFETY: `element` is a live DOM element.
    let element_layout_node = unsafe { (host.callbacks.element_layout_node)(element) };
    !element_layout_node.is_invalid() && state.ancestor_stack.contains(&element_layout_node)
}

fn update_svg_resource(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    resource: *mut c_void,
    graphics_element: *mut c_void,
    layout_node: LayoutNode,
    context: &mut TreeBuilderContext,
    prior_context_value: bool,
) {
    context.layout_svg_mask_or_clip_path = true;
    let prior_has_svg_root = context.has_svg_root;
    context.has_svg_root = true;
    state.ancestor_stack.push(layout_node);

    if !ancestor_stack_contains_element_layout_node(host, state, resource) {
        update_layout_tree(host, state, resource, context, true, FfiInsertionMode::Append);
        // SAFETY: Both pointers denote live SVG elements held by the graphics element.
        unsafe { (host.callbacks.register_svg_resource_reference)(resource, graphics_element) };
    } else {
        // FIXME: Somehow either remove ancestor from the layout tree or mark it as invalid.
    }

    assert!(state.ancestor_stack.pop().is_some());
    context.has_svg_root = prior_has_svg_root;
    context.layout_svg_mask_or_clip_path = prior_context_value;
}

fn update_svg_pattern(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    pattern: *mut c_void,
    content_element: *mut c_void,
    graphics_element: *mut c_void,
    layout_node: LayoutNode,
    context: &mut TreeBuilderContext,
) {
    let prior_context_value = context.layout_svg_pattern;
    context.layout_svg_pattern = true;
    state.ancestor_stack.push(layout_node);

    if !ancestor_stack_contains_element_layout_node(host, state, content_element) {
        update_layout_tree(host, state, content_element, context, true, FfiInsertionMode::Append);
        // The referenced pattern may inherit its content from another pattern via href. Removing either element
        // invalidates the attached resource box, so register the referencer with both.
        // SAFETY: All pointers denote live SVG elements held by the graphics element or pattern chain.
        unsafe {
            (host.callbacks.register_svg_resource_reference)(content_element, graphics_element);
            if pattern != content_element {
                (host.callbacks.register_svg_resource_reference)(pattern, graphics_element);
            }
        }
        // SAFETY: The host arena outlives the tree build.
        unsafe { &*host.arena }.register_svg_pattern_referencing_node(layout_node);
    }

    assert!(state.ancestor_stack.pop().is_some());
    context.layout_svg_pattern = prior_context_value;
}

struct PrincipalDescendantUpdate {
    should_create_layout_node: bool,
    must_create_subtree: bool,
    insertion_mode: FfiInsertionMode,
}

/// Updates the descendants and post-child state of a node with a principal layout box.
///
/// # Safety
///
/// The callback table, DOM node, layout node, and context must remain valid for the duration of the call.
unsafe fn update_principal_node_descendants(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    dom_node: *mut c_void,
    layout_node: LayoutNode,
    context: &mut TreeBuilderContext,
    update: PrincipalDescendantUpdate,
) {
    abort_on_panic(|| {
        let should_create_layout_node = update.should_create_layout_node;
        assert!(!dom_node.is_null());
        assert!(!layout_node.is_invalid());
        let layout_host = host.layout();
        // SAFETY: All pointers remain live throughout the call.
        let facts = unsafe {
            (host.callbacks.principal_descendant_facts)(
                host.callbacks.builder,
                dom_node,
                layout_host.shell(layout_node),
            )
        };
        let (layout_node_can_have_children, layout_node_is_replaced_box_with_children) = {
            let layout_node_data = layout_host.data(layout_node);
            let can_have_children = node_facts::node_can_have_children(layout_node_data);
            (
                can_have_children,
                node_facts::kind_is_replaced_box(layout_node_data.kind.get()) && can_have_children,
            )
        };
        let prior_quote_nesting_level = state.quote_nesting_level;

        if should_create_layout_node {
            // Resolve counters now that we exist in the layout tree.
            if facts.is_element {
                // SAFETY: `dom_node` is a live Element when this fact is set.
                unsafe { (host.callbacks.resolve_counters)(dom_node, FfiPseudoElement::None) };
            }

            // Add the ::before pseudo-element before walking normal children.
            if facts.is_element
                && layout_node_can_have_children
                && !facts.content_visibility_hidden
                && !context.has_svg_root
            {
                state.ancestor_stack.push(layout_node);
                let placed = create_pseudo_element(
                    host,
                    state,
                    dom_node,
                    FfiPseudoElement::Before,
                    Some(FfiInsertionMode::Prepend),
                );
                assert!(placed.is_none());
                assert!(state.ancestor_stack.pop().is_some());
            }
        }

        if facts.content_visibility_hidden {
            // SAFETY: The builder and DOM node remain live throughout the call.
            unsafe {
                (host.callbacks.clear_stale_subtree)(
                    host.callbacks.builder,
                    dom_node,
                    FfiStaleSubtreeClearScope::DescendantsBoundedToRoot,
                );
            }
        }

        if (should_create_layout_node || facts.child_needs_layout_tree_update)
            && (!facts.shadow_root.is_null() || facts.should_layout_dom_children)
            && layout_node_can_have_children
            && !facts.content_visibility_hidden
        {
            state.ancestor_stack.push(layout_node);

            if !facts.shadow_root.is_null() {
                if layout_node_is_replaced_box_with_children {
                    // For replaced elements with shadow DOM children, wrap the children in an
                    // anonymous BlockContainer so that a BFC handles their layout.
                    let first_child = layout_host.first_child(layout_node);
                    if first_child.is_invalid() || !node_has_flag(layout_host.data(first_child), NodeFlag::Anonymous) {
                        let wrapper = layout_host.create_anonymous_wrapper_box(layout_node);
                        layout_host.attach_child(state.current_parent(), wrapper, NodeSlotId::INVALID);
                    }
                    let wrapper = layout_host.first_child(layout_node);
                    assert!(!wrapper.is_invalid());
                    state.ancestor_stack.push(wrapper);
                }
                // SAFETY: The callback table, shadow root, and context remain valid.
                unsafe {
                    update_layout_tree_for_shadow_root_children(
                        host,
                        state,
                        facts.shadow_root,
                        context,
                        should_create_layout_node,
                    );
                }
                if layout_node_is_replaced_box_with_children {
                    assert!(state.ancestor_stack.pop().is_some());
                }
            } else if facts.should_layout_dom_children {
                assert!(!facts.dom_children_parent.is_null());
                if facts.is_svg_switch_element {
                    // SAFETY: The callback table, parent, and context remain valid.
                    unsafe {
                        update_layout_tree_for_svg_switch_children(
                            host,
                            state,
                            facts.dom_children_parent,
                            context,
                            should_create_layout_node,
                        );
                    }
                } else {
                    // SAFETY: The callback table, parent, and context remain valid.
                    unsafe {
                        update_layout_tree_for_dom_children(
                            host,
                            state,
                            facts.dom_children_parent,
                            context,
                            should_create_layout_node,
                            update.insertion_mode,
                        );
                    }
                }
            }

            if facts.is_document {
                // Elements in the top layer do not lay out normally based on their position in the document; instead
                // they generate boxes as if they were siblings of the root element.
                let prior_layout_top_layer = context.layout_top_layer;
                context.layout_top_layer = true;
                // SAFETY: The DOM document remains live and owns a stable top-layer list during this pass.
                let count = unsafe { (host.callbacks.top_layer_element_count)(dom_node) };
                let mut top_layer_elements = vec![std::ptr::null_mut(); count];
                // SAFETY: The output slice has room for the stable top-layer list reported above.
                unsafe {
                    (host.callbacks.copy_top_layer_elements)(dom_node, top_layer_elements.as_mut_ptr(), count);
                }
                for element in top_layer_elements {
                    assert!(!element.is_null());
                    // SAFETY: `element` is a live DOM Element.
                    if !unsafe { (host.callbacks.rendered_in_top_layer)(element) } {
                        continue;
                    }
                    // SAFETY: `element` is a live DOM Element.
                    if has_unrendered_flat_tree_ancestor(host, element) {
                        // SAFETY: The builder and element remain live throughout cleanup.
                        unsafe {
                            (host.callbacks.clear_stale_subtree)(
                                host.callbacks.builder,
                                element,
                                FfiStaleSubtreeClearScope::InclusiveBoundedToRoot,
                            );
                        }
                        continue;
                    }
                    update_layout_tree(
                        host,
                        state,
                        element,
                        context,
                        should_create_layout_node,
                        FfiInsertionMode::Append,
                    );
                }
                context.layout_top_layer = prior_layout_top_layer;
            }

            assert!(state.ancestor_stack.pop().is_some());
        }

        if !facts.slot_element.is_null() {
            if !facts.content_visibility_hidden {
                state.ancestor_stack.push(layout_node);
                // SAFETY: The callback table, slot element, and context remain valid.
                unsafe {
                    update_layout_tree_for_assigned_slottables(
                        host,
                        state,
                        facts.slot_element,
                        context,
                        update.must_create_subtree || should_create_layout_node,
                    );
                }
                assert!(state.ancestor_stack.pop().is_some());
            } else {
                clear_stale_assigned_slottables(
                    facts.slot_element,
                    |slot| unsafe { (host.callbacks.assigned_node_count)(slot) },
                    |slot, index| unsafe { (host.callbacks.assigned_node_at)(slot, index) },
                    |root| unsafe {
                        (host.callbacks.clear_stale_subtree)(
                            host.callbacks.builder,
                            root,
                            FfiStaleSubtreeClearScope::InclusiveBoundedToRoot,
                        );
                    },
                );
            }
        }

        if should_create_layout_node {
            if !facts.svg_graphics_element.is_null() {
                for resource in [facts.svg_mask, facts.svg_clip_path] {
                    if !resource.is_null() {
                        update_svg_resource(
                            host,
                            state,
                            resource,
                            facts.svg_graphics_element,
                            layout_node,
                            context,
                            context.layout_svg_mask_or_clip_path,
                        );
                    }
                }

                let mut seen_content_elements = Vec::with_capacity(2);
                for pattern in [facts.svg_fill_pattern, facts.svg_stroke_pattern] {
                    if pattern.is_null() {
                        continue;
                    }
                    // SAFETY: `pattern` is a live SVGPatternElement.
                    let content_element = unsafe { (host.callbacks.svg_pattern_content_element)(pattern) };
                    if content_element.is_null() || seen_content_elements.contains(&content_element) {
                        continue;
                    }
                    seen_content_elements.push(content_element);
                    update_svg_pattern(
                        host,
                        state,
                        pattern,
                        content_element,
                        facts.svg_graphics_element,
                        layout_node,
                        context,
                    );
                }
            }

            // Add ::marker and ::after once normal and SVG resource children are complete.
            if facts.is_element
                && layout_node_can_have_children
                && !facts.content_visibility_hidden
                && !context.has_svg_root
            {
                state.ancestor_stack.push(layout_node);
                if layout_host.data(layout_node).kind.get() == NodeKind::ListItemBox {
                    let placed = create_pseudo_element(
                        host,
                        state,
                        dom_node,
                        FfiPseudoElement::Marker,
                        Some(FfiInsertionMode::Prepend),
                    );
                    assert!(placed.is_none());
                }
                let placed = create_pseudo_element(
                    host,
                    state,
                    dom_node,
                    FfiPseudoElement::After,
                    Some(FfiInsertionMode::Append),
                );
                assert!(placed.is_none());
                assert!(state.ancestor_stack.pop().is_some());

                // SAFETY: The layout node's shell and its associated DOM node remain live throughout the call.
                if node_kind_is_block_container(layout_host.data(layout_node).kind.get())
                    && unsafe { (host.callbacks.layout_node_has_first_letter_style)(layout_host.shell(layout_node)) }
                {
                    let target = find_first_letter_in_block(host, layout_node);
                    if target.found {
                        create_first_letter_boxes(host, dom_node, target);
                    }
                }
            }

            let layout_host = host.layout();
            wrap_fieldset_contents_if_needed(&layout_host, layout_node);
            wrap_button_contents_if_needed(&layout_host, layout_node);
        }

        // https://www.w3.org/TR/css-contain-2/#containment-style
        // Giving an element style containment has the following effects:
        // 2. The effects of the 'content' property’s 'open-quote', 'close-quote', 'no-open-quote' and 'no-close-quote'
        //    must be scoped to the element’s sub-tree.
        if node_facts::node_style_view(host.layout().data(layout_node))
            .is_some_and(crate::painting::style_queries::has_style_containment)
        {
            state.quote_nesting_level = prior_quote_nesting_level;
        }

        // SAFETY: `dom_node` remains live throughout the call.
        unsafe { (host.callbacks.clear_dom_update_flags)(dom_node) };
    });
}

struct PrincipalNodeUpdate<'host, 'callbacks, 'state, 'context> {
    host: &'host DomTreeBuilderHost<'callbacks>,
    state: &'state mut TreeBuilderState,
    frame: *mut c_void,
    old_layout_node: LayoutNode,
    dom_node: *mut c_void,
    context: &'context mut TreeBuilderContext,
    must_create_subtree: bool,
    insertion_mode: FfiInsertionMode,
}

struct PrincipalBoxConstruction {
    layout_node: LayoutNode,
    created_box: Option<UnplacedLayoutNode>,
    handled_display_contents: bool,
}

impl PrincipalBoxConstruction {
    fn none() -> Self {
        Self {
            layout_node: NodeSlotId::INVALID,
            created_box: None,
            handled_display_contents: false,
        }
    }
}

fn construct_principal_layout_node(
    update: &mut PrincipalNodeUpdate<'_, '_, '_, '_>,
    entry_facts: FfiPrincipalNodeEntryFacts,
    should_create_layout_node: bool,
) -> PrincipalBoxConstruction {
    let host = update.host;
    let mut created_box = None;
    let frame = update.frame;
    let dom_node = update.dom_node;
    let must_create_subtree = update.must_create_subtree;
    let context = &mut *update.context;
    if entry_facts.is_element {
        if should_create_layout_node {
            // ::backdrop is a sibling of the element, not a child, so unlike other pseudo-elements, it is not
            // automatically discarded when the element's layout is recomputed.
            // A stale ::backdrop box is a viewport child, so removing it restructures the tree outside
            // every rebuild root.
            // SAFETY: The DOM element remains live throughout the call.
            let old_backdrop =
                unsafe { (host.callbacks.element_pseudo_layout_node)(dom_node, FfiPseudoElement::Backdrop) };
            if !old_backdrop.is_invalid() {
                update.state.layout_tree_update_escaped_rebuild_roots = true;
                let layout_host = host.layout();
                let backdrop_parent = layout_host.parent(old_backdrop);
                assert!(!backdrop_parent.is_invalid());
                layout_host.arena().detach_child(backdrop_parent, old_backdrop);
                layout_host.free_subtree(old_backdrop);
            }
        }
        // SAFETY: The frame, builder, and DOM element remain live throughout the call.
        let prepared = unsafe {
            (host.callbacks.prepare_principal_element)(
                host.callbacks.builder,
                frame,
                dom_node,
                should_create_layout_node,
            )
        };
        let generation = principal_box_generation_decision(
            true,
            should_create_layout_node && prepared.display.display_is_none,
            prepared.display.display_is_contents,
        );
        if generation == PrincipalBoxGenerationDecision::Suppress {
            return PrincipalBoxConstruction::none();
        }
        if generation == PrincipalBoxGenerationDecision::DisplayContents {
            // SAFETY: The callback table, DOM element, and context remain valid throughout the recursive walk.
            unsafe {
                update_layout_tree_for_display_contents(
                    host,
                    update.state,
                    dom_node,
                    context,
                    must_create_subtree,
                    should_create_layout_node,
                );
            }
            return PrincipalBoxConstruction {
                handled_display_contents: true,
                ..PrincipalBoxConstruction::none()
            };
        }
        if should_create_layout_node {
            // SAFETY: The frame and element remain live throughout construction.
            let layout_facts = unsafe { (host.callbacks.principal_element_layout_facts)(frame, dom_node) };
            let layout_kind = element_layout_kind(
                layout_facts,
                context.layout_svg_mask_or_clip_path,
                context.layout_svg_pattern,
            );
            // SAFETY: The builder, frame, and element remain live throughout construction.
            let created = unsafe {
                (host.callbacks.create_principal_element_layout)(host.callbacks.builder, frame, dom_node, layout_kind)
            };
            if !created.is_invalid() {
                created_box = Some(host.layout().created(created));
            }
            if matches!(
                layout_kind,
                FfiElementLayoutKind::SvgMask | FfiElementLayoutKind::SvgClipPath
            ) {
                // Only direct mask and clip-path uses inherit this construction mode.
                context.layout_svg_mask_or_clip_path = false;
            } else if layout_kind == FfiElementLayoutKind::SvgPattern {
                // Only the directly referenced pattern inherits this construction mode.
                context.layout_svg_pattern = false;
            }
        } else {
            // SAFETY: The frame and DOM node remain live throughout the call.
            unsafe { (host.callbacks.reuse_principal_layout)(frame, dom_node) };
        }
    } else if should_create_layout_node {
        if entry_facts.is_document {
            // SAFETY: The frame and DOM document remain live throughout construction.
            let created = unsafe { (host.callbacks.create_principal_document_layout)(frame, dom_node) };
            created_box = Some(host.layout().created(created));
        } else if entry_facts.is_text {
            // SAFETY: The DOM text node remains live throughout the fact query.
            let facts = unsafe { (host.callbacks.principal_text_layout_facts)(dom_node) };
            let needs_style_wrapper = display_contents_text_needs_style_wrapper(
                facts.has_style_parent,
                facts.parent_display_is_contents,
                facts.text_is_ascii_whitespace,
                facts.parent_collapses_whitespace,
            );
            // SAFETY: The frame and DOM text node remain live throughout construction.
            let text_layout_node = unsafe { (host.callbacks.create_principal_text_layout)(frame, dom_node) };
            let layout_host = host.layout();
            if needs_style_wrapper {
                let wrapper = layout_host.create_anonymous_box_from_style_record(
                    facts.style_parent_style_record,
                    FfiAnonymousStyleKind::InlineStyleWrapper,
                    FfiAnonymousStyleOverrides::default(),
                    NodeKind::InlineNode,
                );
                let wrapper_slot = wrapper.slot();
                layout_host.set_children_are_inline(wrapper_slot, true);
                layout_host.attach_child(wrapper_slot, layout_host.created(text_layout_node), NodeSlotId::INVALID);
                // SAFETY: The builder and frame remain live, and the wrapper is a live node the builder owns.
                unsafe { (host.callbacks.set_principal_layout_node)(host.callbacks.builder, frame, wrapper_slot) };
                created_box = Some(wrapper);
            } else {
                created_box = Some(layout_host.created(text_layout_node));
            }
        }
    } else {
        // SAFETY: The frame and DOM node remain live throughout the call.
        unsafe { (host.callbacks.reuse_principal_layout)(frame, dom_node) };
    }

    // SAFETY: The frame remains live throughout the call.
    let layout_node = unsafe { (host.callbacks.principal_layout_node)(frame) };
    PrincipalBoxConstruction {
        layout_node,
        created_box,
        handled_display_contents: false,
    }
}

// The replacement box represents the same element in the same tree position, so the flat fragment
// and inline-box-piece lists held by the containing block of a node that participated in inline
// layout carry over to it; a subtree relayout that skips the containing block never rebuilds them.
fn transfer_fragments_to_replacement_box(
    arena: &LayoutNodeArena,
    old_layout_node: LayoutNode,
    new_layout_node: LayoutNode,
) {
    let Some(containing_block) = arena.node_containing_block_if_live(old_layout_node) else {
        return;
    };
    if !arena.slot_is_live(containing_block) {
        return;
    }
    let paintable_rows = arena.paintable_rows();
    if !paintable_rows.paintable_row_is_populated(containing_block)
        || !crate::painting::node_painting::has_lines(&paintable_rows, containing_block)
    {
        return;
    }
    arena.transfer_fragments_to_replacement_node(containing_block, old_layout_node, new_layout_node);
}

fn update_principal_node_after_entry(
    update: &mut PrincipalNodeUpdate<'_, '_, '_, '_>,
    entry_facts: FfiPrincipalNodeEntryFacts,
    entry_decision: PrincipalNodeEntryDecision,
) {
    let host = update.host;
    let frame = update.frame;
    let dom_node = update.dom_node;

    let prior_has_svg_root = update.context.has_svg_root;
    match entry_decision.svg {
        SvgEntryDecision::EnterSvgRoot => update.context.has_svg_root = true,
        SvgEntryDecision::EnterForeignContent => update.context.has_svg_root = false,
        SvgEntryDecision::Continue | SvgEntryDecision::Skip => {}
    }

    let construction = if entry_decision.svg == SvgEntryDecision::Skip {
        PrincipalBoxConstruction::none()
    } else {
        construct_principal_layout_node(update, entry_facts, entry_decision.should_create_layout_node)
    };
    let mut created_box = construction.created_box;
    let context = &mut *update.context;

    if !construction.layout_node.is_invalid() {
        if entry_facts.is_element || entry_facts.is_document {
            // SAFETY: The frame owns a live NodeWithStyle for elements and documents.
            unsafe { (host.callbacks.attach_principal_style_resources)(frame) };
        }

        // SAFETY: `has_layout_node` guarantees that the frame owns a live principal layout node.
        let layout_node = unsafe { (host.callbacks.principal_layout_node)(frame) };
        let starts_new_subtree = entry_decision.should_create_layout_node && update.state.new_subtree_root.is_invalid();
        if starts_new_subtree {
            update.state.new_subtree_root = layout_node;
        }
        if entry_facts.needs_layout_tree_update
            && entry_facts.may_reuse_layout_node_for_child_list_insertion
            && !entry_decision.should_create_layout_node
        {
            update.state.reused_child_list_update_roots.push(layout_node);
        }
        let adjustment = replaced_element_display_adjustment(&host.layout(), layout_node);
        if adjustment != FfiReplacedElementDisplayAdjustment::None {
            // SAFETY: The frame owns a live NodeWithStyle.
            unsafe { (host.callbacks.apply_replaced_display_adjustment)(frame, adjustment) };
        }

        let old_layout_node = update.old_layout_node;
        let placement_facts = PrincipalBoxPlacementFacts {
            must_create_subtree: update.must_create_subtree,
            should_create_layout_node: entry_decision.should_create_layout_node,
            has_old_layout_node: !old_layout_node.is_invalid(),
            old_layout_node_is_attached: !old_layout_node.is_invalid()
                && !host.layout().parent(old_layout_node).is_invalid(),
            old_and_new_layout_nodes_are_same: old_layout_node == layout_node,
            has_current_rebuild_root: !update.state.current_rebuild_root.is_invalid(),
            is_in_dom_order_insertion: update.insertion_mode == FfiInsertionMode::InDomOrder,
            is_document: entry_facts.is_document,
            is_element: entry_facts.is_element,
            rendered_in_top_layer: entry_facts.rendered_in_top_layer,
        };
        let layout_node_is_svg_box = node_kind_is_svg_box(host.layout().data(layout_node).kind.get());
        let prior_layout_top_layer = context.layout_top_layer;
        let placement =
            principal_box_placement_decision(placement_facts, layout_node_is_svg_box, prior_layout_top_layer);

        let mut prior_rebuild_root = NodeSlotId::INVALID;
        if placement.start_rebuild_root {
            prior_rebuild_root = update.state.current_rebuild_root;
            update.state.current_rebuild_root = layout_node;
            // The shell pointer is captured now, while the box is known to be live; the reported
            // list mirrors what the bridge used to append at this exact point.
            update
                .state
                .rebuilt_subtree_root_shells
                .push(host.layout().shell(layout_node));
            update.state.rebuilt_subtree_roots.push(layout_node);
        } else if placement.mark_update_escaped_rebuild_roots {
            update.state.layout_tree_update_escaped_rebuild_roots = true;
        }

        if placement.create_backdrop {
            // A backdrop is a sibling of its originating top-layer element. Append it normally, but insert it before
            // the placement of an old box that will be replaced in place so the backdrop remains behind the element.
            let insertion_mode = if placement.may_replace_existing_layout_node {
                None
            } else {
                Some(FfiInsertionMode::Append)
            };
            let unplaced_backdrop =
                create_pseudo_element(host, update.state, dom_node, FfiPseudoElement::Backdrop, insertion_mode);
            if let Some(backdrop) = unplaced_backdrop {
                assert!(placement.may_replace_existing_layout_node);
                let layout_host = host.layout();
                let topmost_placement = topmost_layout_node_of_top_layer_placement(host.arena, old_layout_node);
                let old_placement = if topmost_placement.is_invalid() {
                    old_layout_node
                } else {
                    topmost_placement
                };
                let old_parent = layout_host.parent(old_placement);
                assert!(!old_parent.is_invalid());
                // The backdrop lands next to the still-attached old placement, so this restructures
                // its parent.
                note_layout_tree_restructuring_at(&layout_host, update.state, old_parent);
                layout_host.attach_child(old_parent, backdrop, old_placement);
            }
        }

        if placement.clear_layout_top_layer_for_descendants {
            context.layout_top_layer = false;
        }

        let current_parent = if update.state.ancestor_stack.is_empty() {
            NodeSlotId::INVALID
        } else {
            update.state.current_parent()
        };
        match placement.placement {
            FfiPrincipalBoxPlacement::NormalInsertion => {
                let is_inline_outside = node_is_inline_outside(&host.layout(), layout_node);
                insert_node_into_inline_or_block_ancestor(
                    host,
                    update.state,
                    current_parent,
                    created_box.take().expect("a principal box to place"),
                    is_inline_outside,
                    update.insertion_mode,
                    dom_node,
                );
            }
            FfiPrincipalBoxPlacement::AppendSvg => {
                assert!(!current_parent.is_invalid());
                host.layout().attach_child(
                    current_parent,
                    created_box.take().expect("a principal box to place"),
                    NodeSlotId::INVALID,
                );
            }
            FfiPrincipalBoxPlacement::ReplaceExisting => {
                let layout_host = host.layout();
                let arena = layout_host.arena();
                let old_data = arena.data(old_layout_node);
                let new_data = arena.data(layout_node);
                if node_kind_is_box(old_data.kind.get())
                    && node_kind_is_box(new_data.kind.get())
                    && let Some(inputs) = arena.saved_abspos_layout_inputs(old_data)
                {
                    arena.set_saved_abspos_layout_inputs(new_data, Some(inputs));
                }
                if node_kind_is_box(old_data.kind.get())
                    && node_kind_is_box(new_data.kind.get())
                    && let Some(link) = arena.take_committed_fragment_link(old_data)
                {
                    arena.set_committed_fragment_link(new_data, link);
                }
                transfer_fragments_to_replacement_box(arena, old_layout_node, layout_node);
                // SAFETY: The frame retains the attached old layout node.
                unsafe {
                    (layout_host.callbacks.prepare_subtree_for_detach)(
                        layout_host.callbacks.context,
                        layout_host.shell(old_layout_node),
                    );
                }
                let old_parent = layout_host.parent(old_layout_node);
                assert!(!old_parent.is_invalid());
                let replaced_old_box = arena.replace_child(
                    old_parent,
                    old_layout_node,
                    created_box.take().expect("a principal box to place"),
                );
                layout_host.free_subtree(replaced_old_box);
            }
            FfiPrincipalBoxPlacement::DocumentRoot => {
                // SAFETY: The builder and frame remain live; the frame retains the viewport.
                unsafe { (host.callbacks.set_layout_root)(host.callbacks.builder, frame) };
                if let Some(viewport) = created_box.take() {
                    viewport.placed_as_layout_root();
                }
            }
            FfiPrincipalBoxPlacement::None => assert!(created_box.is_none()),
        }
        // SAFETY: The callback table, DOM node, layout node, and context remain live throughout the call.
        unsafe {
            update_principal_node_descendants(
                host,
                update.state,
                dom_node,
                (host.callbacks.principal_layout_node)(frame),
                context,
                PrincipalDescendantUpdate {
                    should_create_layout_node: entry_decision.should_create_layout_node,
                    must_create_subtree: update.must_create_subtree,
                    insertion_mode: if entry_facts.may_reuse_layout_node_for_child_list_insertion {
                        FfiInsertionMode::InDomOrder
                    } else {
                        FfiInsertionMode::Append
                    },
                },
            );
        }

        if placement.clear_layout_top_layer_for_descendants {
            context.layout_top_layer = prior_layout_top_layer;
        }
        if placement.start_rebuild_root {
            update.state.current_rebuild_root = prior_rebuild_root;
        }
        if starts_new_subtree {
            update.state.new_subtree_root = NodeSlotId::INVALID;
        }
    } else if !construction.handled_display_contents {
        if !update.old_layout_node.is_invalid() {
            let old_parent = host.layout().parent(update.old_layout_node);
            if !old_parent.is_invalid() {
                update.state.additional_table_fixup_roots.push(old_parent);
            }
        }
        // If no layout node was created, remove every stale layout and paint node from the shadow-including subtree.
        // SAFETY: The builder and DOM node remain live throughout cleanup.
        unsafe {
            (host.callbacks.clear_stale_subtree)(
                host.callbacks.builder,
                dom_node,
                FfiStaleSubtreeClearScope::Inclusive,
            );
        }
    }

    if matches!(
        entry_decision.svg,
        SvgEntryDecision::EnterSvgRoot | SvgEntryDecision::EnterForeignContent
    ) {
        context.has_svg_root = prior_has_svg_root;
    }
}

/// Updates one DOM node and its layout-tree subtree.
///
/// # Safety
///
/// The callback table and DOM node must remain valid for the duration of the call.
fn update_layout_tree(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    dom_node: *mut c_void,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
    insertion_mode: FfiInsertionMode,
) {
    abort_on_panic(|| {
        assert!(!dom_node.is_null());
        // SAFETY: The builder and DOM node remain live throughout the call.
        let entry_facts = unsafe {
            (host.callbacks.principal_node_entry_facts)(host.callbacks.builder, dom_node, must_create_subtree)
        };
        let entry_decision = principal_node_entry_decision(entry_facts, context);
        if entry_decision.top_layer != TopLayerEntryDecision::Continue {
            if entry_decision.top_layer == TopLayerEntryDecision::SkipAndRequestZoneRebuild {
                // A member found here without an attached box was cleared together with a hidden ancestor subtree, and
                // nothing is scheduled to rebuild it. Request another top-layer zone pass instead of stranding dirty
                // flags below ancestors whose walks already finished.
                // SAFETY: `dom_node` remains live throughout the call.
                unsafe { (host.callbacks.request_top_layer_zone_rebuild)(dom_node) };
            }
            return;
        }

        // SAFETY: The builder and DOM node remain live, and the callback retains frame-owned C++ objects.
        let pushed_frame = unsafe { (host.callbacks.push_principal_frame)(host.callbacks.builder, dom_node) };
        assert!(!pushed_frame.frame.is_null());
        let mut update = PrincipalNodeUpdate {
            host,
            state,
            frame: pushed_frame.frame,
            old_layout_node: pushed_frame.old_layout_node,
            dom_node,
            context,
            must_create_subtree,
            insertion_mode,
        };
        update_principal_node_after_entry(&mut update, entry_facts, entry_decision);
        // SAFETY: `frame` is the most recently pushed principal frame and is no longer used by Rust.
        unsafe { (host.callbacks.pop_principal_frame)(host.callbacks.builder, pushed_frame.frame) };
    });
}

/// Builds or incrementally updates a document's layout tree and applies table fixup.
///
/// # Safety
///
/// The callback table, arena, and document must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_layout_tree(
    callbacks: *const FfiDomTreeBuilderCallbacks,
    arena: *mut c_void,
    document: *mut c_void,
) {
    assert!(!document.is_null());
    // SAFETY: Guaranteed by the entry point's contract.
    let host = unsafe { dom_tree_builder_host(callbacks, arena) };
    let mut state = TreeBuilderState::default();
    let mut context = TreeBuilderContext::default();
    // SAFETY: All pointers remain live throughout the build.
    let entry_facts = unsafe { (host.callbacks.principal_node_entry_facts)(host.callbacks.builder, document, false) };
    assert!(entry_facts.is_document);

    update_layout_tree(
        &host,
        &mut state,
        document,
        &mut context,
        false,
        FfiInsertionMode::Append,
    );

    // NB: Called during layout tree construction.
    // SAFETY: The document remains live and any attached layout root is owned by it and the builder.
    let document_layout_node = unsafe { (host.callbacks.document_layout_node)(document) };
    if !document_layout_node.is_invalid() {
        let layout_host = host.layout();
        if entry_facts.document_needs_full_layout_tree_update
            || !entry_facts.has_layout_node
            || state.layout_tree_update_escaped_rebuild_roots
        {
            fixup_tables(&layout_host, document_layout_node);
        } else {
            fixup_tables_in_rebuilt_subtrees(
                &layout_host,
                &state.rebuilt_subtree_roots,
                &state.reused_child_list_update_roots,
                &state.additional_table_fixup_roots,
            );
        }
    }

    for &element in &state.layout_tree_rebuild_requests {
        // SAFETY: The builder remains live, and the walk that could clear DOM update flags is complete.
        unsafe { (host.callbacks.request_layout_tree_rebuild)(host.callbacks.builder, element) };
    }

    // SAFETY: The builder remains live and copies the reported shell pointers before returning.
    unsafe {
        (host.callbacks.report_rebuild_outcome)(
            host.callbacks.builder,
            state.rebuilt_subtree_root_shells.as_ptr(),
            state.rebuilt_subtree_root_shells.len(),
            state.layout_tree_update_escaped_rebuild_roots,
        );
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiReplacedElementDisplayAdjustment {
    None,
    Inline,
    Block,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// NB: `Other` is constructed by C++ through the FFI.
#[allow(dead_code)]
pub enum FfiPseudoElement {
    Before,
    After,
    Marker,
    Backdrop,
    Other,
    // Marks counter resolution against the element itself rather than one of its pseudo-elements.
    None,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// NB: `List` is constructed by C++ through the FFI.
#[allow(dead_code)]
pub enum FfiComputedContentType {
    Normal,
    None,
    List,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiPseudoElementDecision {
    None,
    ContentReplacement,
    Contents,
    Box,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPseudoElementFacts {
    pub has_style: bool,
    pub pseudo_element: FfiPseudoElement,
    pub content_type: FfiComputedContentType,
    pub display_is_none: bool,
    pub display_is_contents: bool,
    pub display_is_list_item: bool,
    pub has_content_replacement: bool,
    pub originating_layout_node_is_list_item: bool,
    pub normal_marker_has_content: bool,
    pub marker_position_is_inside: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiResolvedPseudoContentFacts {
    pub final_quote_nesting_level: u32,
    pub content_is_list: bool,
    pub content_item_count: usize,
}

#[repr(C)]
pub struct FfiPseudoTreeBuilderCallbacks {
    pub builder: *mut c_void,
    pub push_frame: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub pop_frame: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub initialize: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiPseudoElement) -> FfiPseudoElementFacts,
    pub create_layout_node: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        *mut c_void,
        FfiPseudoElement,
        FfiPseudoElementDecision,
    ) -> NodeSlotId,
    pub attach_style_resources: unsafe extern "C" fn(*mut c_void),
    pub apply_replaced_display_adjustment: unsafe extern "C" fn(*mut c_void, FfiReplacedElementDisplayAdjustment),
    pub create_nested_list_marker: unsafe extern "C" fn(*mut c_void, *mut c_void) -> NodeSlotId,
    pub create_nested_list_marker_content:
        unsafe extern "C" fn(*mut c_void, *mut c_void, FfiPseudoElement, *mut c_void) -> NodeSlotId,
    pub configure_layout_node: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiPseudoElement),
    pub resolve_content:
        unsafe extern "C" fn(*mut c_void, *mut c_void, FfiPseudoElement, u32) -> FfiResolvedPseudoContentFacts,
    pub create_content_item: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiPseudoElement, usize) -> NodeSlotId,
}

pub(crate) fn pseudo_element_decision(facts: FfiPseudoElementFacts) -> FfiPseudoElementDecision {
    abort_on_panic(|| {
        if !facts.has_style {
            return FfiPseudoElementDecision::None;
        }

        // https://drafts.csswg.org/css-display-3/#box-generation
        // The element and its descendants generate no boxes or text sequences.
        if facts.display_is_none {
            return FfiPseudoElementDecision::None;
        }

        // ::before and ::after only exist if they have content. `content: normal` computes to `none` for them.
        if matches!(facts.pseudo_element, FfiPseudoElement::Before | FfiPseudoElement::After)
            && matches!(
                facts.content_type,
                FfiComputedContentType::Normal | FfiComputedContentType::None
            )
        {
            return FfiPseudoElementDecision::None;
        }

        // For ::marker with content 'none' -- do nothing.
        if facts.pseudo_element == FfiPseudoElement::Marker && facts.content_type == FfiComputedContentType::None {
            return FfiPseudoElementDecision::None;
        }

        if facts.pseudo_element == FfiPseudoElement::Marker
            && facts.content_type == FfiComputedContentType::Normal
            && facts.originating_layout_node_is_list_item
        {
            // https://www.w3.org/TR/css-lists-3/#content-property
            // "::marker does not generate a box" when list-style-type is 'none' and there's no marker image. Custom
            // ::marker content is already excluded by the outer condition checking for Type::Normal.
            return if facts.normal_marker_has_content {
                FfiPseudoElementDecision::Box
            } else {
                FfiPseudoElementDecision::None
            };
        }

        // https://drafts.csswg.org/css-content-3/#content-property
        // Note: If the value of <content-list> is a single <image>, it must instead be interpreted as a
        // <content-replacement>.
        // Makes the element or pseudo-element a replaced element, filled with the specified <image>.
        let mut is_content_replacement = facts.has_content_replacement;

        // INTEROP: Blink, WebKit, and Gecko keep generated images as children of pseudo-element boxes. Preserve that
        //          behavior for list items because our marker layout currently requires a ListItemBox.
        if facts.display_is_list_item {
            is_content_replacement = false;
        }

        // https://drafts.csswg.org/css-display-3/#box-generation
        // This value computes to 'display: none' on replaced elements.
        // INTEROP: Blink, WebKit, and Gecko preserve image content on 'display: contents' pseudo-elements instead.
        if facts.display_is_contents {
            is_content_replacement = false;
        }

        if is_content_replacement {
            FfiPseudoElementDecision::ContentReplacement
        } else if facts.display_is_contents {
            FfiPseudoElementDecision::Contents
        } else {
            FfiPseudoElementDecision::Box
        }
    })
}

fn create_pseudo_element(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    element: *mut c_void,
    pseudo_element: FfiPseudoElement,
    insertion_mode: Option<FfiInsertionMode>,
) -> Option<UnplacedLayoutNode> {
    assert!(!element.is_null());
    let callbacks = &host.callbacks.pseudo;
    // SAFETY: The builder owns frame storage that remains live throughout the build.
    let frame = unsafe { (callbacks.push_frame)(callbacks.builder) };
    assert!(!frame.is_null());
    let unplaced_box = create_pseudo_element_with_frame(host, state, frame, element, pseudo_element, insertion_mode);
    // SAFETY: `frame` is the most recently pushed pseudo-element frame and Rust no longer uses it.
    unsafe { (callbacks.pop_frame)(callbacks.builder, frame) };
    unplaced_box
}

fn create_pseudo_element_with_frame(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    frame: *mut c_void,
    element: *mut c_void,
    pseudo_element: FfiPseudoElement,
    insertion_mode: Option<FfiInsertionMode>,
) -> Option<UnplacedLayoutNode> {
    let callbacks = &host.callbacks.pseudo;
    // SAFETY: The frame and element remain live throughout initialization.
    let facts = unsafe { (callbacks.initialize)(frame, element, pseudo_element) };
    let decision = pseudo_element_decision(facts);
    if decision == FfiPseudoElementDecision::None {
        return None;
    }

    // SAFETY: The builder, frame, and element remain live throughout construction.
    let layout_node =
        unsafe { (callbacks.create_layout_node)(callbacks.builder, frame, element, pseudo_element, decision) };
    if layout_node.is_invalid() {
        return None;
    }
    let layout_host = host.layout();
    let mut unplaced_box = Some(layout_host.created(layout_node));

    // https://drafts.csswg.org/css-lists-3/#list-style-position-outside
    // "the marker box is a block container and is placed outside the principal block box"
    if decision == FfiPseudoElementDecision::Box
        && facts.originating_layout_node_is_list_item
        && !facts.marker_position_is_inside
    {
        // SAFETY: The originating element remains live and owns a list item box.
        let list_item_box = unsafe { (host.callbacks.element_layout_node)(element) };
        assert_eq!(layout_host.data(list_item_box).kind.get(), NodeKind::ListItemBox);
        let first_child = layout_host.first_child(list_item_box);
        layout_host.attach_child(list_item_box, unplaced_box.take().expect("the marker box"), first_child);
    }

    // SAFETY: The frame owns a live pseudo-element layout node.
    unsafe { (callbacks.attach_style_resources)(frame) };
    if decision == FfiPseudoElementDecision::ContentReplacement {
        let adjustment = replaced_element_display_adjustment(&host.layout(), layout_node);
        if adjustment != FfiReplacedElementDisplayAdjustment::None {
            // SAFETY: The frame owns a live NodeWithStyle.
            unsafe { (callbacks.apply_replaced_display_adjustment)(frame, adjustment) };
        }
    }

    let initial_quote_nesting_level = state.quote_nesting_level;
    // SAFETY: The frame and element remain live throughout configuration.
    unsafe { (callbacks.configure_layout_node)(frame, element, pseudo_element) };
    let layout_node_kind = layout_host.data(layout_node).kind.get();
    let is_outside_marker = layout_node_kind == NodeKind::ListItemMarkerBox && !facts.marker_position_is_inside;
    if let Some(insertion_mode) = insertion_mode
        && !is_outside_marker
    {
        let current_parent = state.current_parent();
        let is_inline_outside = node_is_inline_outside(&layout_host, layout_node);
        insert_node_into_inline_or_block_ancestor(
            host,
            state,
            current_parent,
            unplaced_box.take().expect("the pseudo-element box"),
            is_inline_outside,
            insertion_mode,
            std::ptr::null_mut(),
        );
    }
    // SAFETY: The element remains live and its pseudo-element layout node is attached when requested.
    unsafe { (host.callbacks.resolve_counters)(element, pseudo_element) };

    // FIXME: This code actually computes style for element::marker, and shouldn't for element::pseudo::marker.
    if layout_node_kind == NodeKind::ListItemBox {
        // SAFETY: The frame and element remain live throughout marker creation.
        let marker = layout_host.created(unsafe { (callbacks.create_nested_list_marker)(frame, element) });
        let marker_slot = marker.slot();
        let first_child = layout_host.first_child(layout_node);
        layout_host.attach_child(layout_node, marker, first_child);
        // SAFETY: The frame, element, and marker remain live throughout content creation.
        let content = unsafe {
            (callbacks.create_nested_list_marker_content)(
                frame,
                element,
                pseudo_element,
                layout_host.shell(marker_slot),
            )
        };
        if !content.is_invalid() {
            layout_host.attach_child(marker_slot, layout_host.created(content), NodeSlotId::INVALID);
        }
        layout_host.set_children_are_inline(marker_slot, true);
    }

    // Resolve content after insertion because counter() and counters() items read the counters established by this
    // pseudo-element's box.
    // SAFETY: The frame and element remain live throughout content resolution.
    let resolved_content =
        unsafe { (callbacks.resolve_content)(frame, element, pseudo_element, initial_quote_nesting_level) };
    state.quote_nesting_level = resolved_content.final_quote_nesting_level;

    if resolved_content.content_is_list && decision != FfiPseudoElementDecision::ContentReplacement {
        state.ancestor_stack.push(layout_node);
        for index in 0..resolved_content.content_item_count {
            // SAFETY: `index` is below the resolved content item count and the frame retains any returned node.
            let content_item = unsafe { (callbacks.create_content_item)(frame, element, pseudo_element, index) };
            if content_item.is_invalid() {
                continue;
            }
            let current_parent = state.current_parent();
            let is_inline_outside = node_is_inline_outside(&layout_host, content_item);
            insert_node_into_inline_or_block_ancestor(
                host,
                state,
                current_parent,
                layout_host.created(content_item),
                is_inline_outside,
                FfiInsertionMode::Append,
                std::ptr::null_mut(),
            );
        }
        assert!(state.ancestor_stack.pop().is_some());
    }

    unplaced_box
}

fn replaced_element_display_adjustment(
    host: &TreeBuilderHost<'_>,
    node: LayoutNode,
) -> FfiReplacedElementDisplayAdjustment {
    if !node_has_flag(host.data(node), NodeFlag::IsReplacedElement) {
        return FfiReplacedElementDisplayAdjustment::None;
    }
    let display = host.display(node);
    adjusted_table_display_for_replaced_element(
        display.is_table_inside(),
        !host
            .style(node)
            .is_some_and(|style| style.display().is_inline_outside()),
        display.is_internal_table(),
        display.is_table_caption(),
    )
}

pub(crate) fn adjusted_table_display_for_replaced_element(
    is_table_inside: bool,
    is_block_outside: bool,
    is_internal_table: bool,
    is_table_caption: bool,
) -> FfiReplacedElementDisplayAdjustment {
    abort_on_panic(|| {
        // https://drafts.csswg.org/css-display-3/#outer-role
        // Note: Outer display types do affect replaced elements.
        if is_table_inside {
            if is_block_outside {
                return FfiReplacedElementDisplayAdjustment::Block;
            }
            return FfiReplacedElementDisplayAdjustment::Inline;
        }

        // https://drafts.csswg.org/css-display-3/#layout-specific-display
        // When the 'display' property of a replaced element computes to one of the layout-internal values, it is
        // handled as having a used value of 'display: inline'.
        if is_internal_table || is_table_caption {
            return FfiReplacedElementDisplayAdjustment::Inline;
        }
        FfiReplacedElementDisplayAdjustment::None
    })
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiFirstLetterTarget {
    pub text_node: *mut c_void,
    pub text_layout_node: NodeSlotId,
    pub letter_start: usize,
    pub letter_end: usize,
    pub source_length: usize,
    pub found: bool,
}

impl FfiFirstLetterTarget {
    fn not_found() -> Self {
        Self {
            text_node: std::ptr::null_mut(),
            text_layout_node: NodeSlotId::INVALID,
            letter_start: 0,
            letter_end: 0,
            source_length: 0,
            found: false,
        }
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiFirstLetterNodes {
    pub wrapper: NodeSlotId,
    pub first_letter_slice: NodeSlotId,
    pub remainder_slice: NodeSlotId,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiCodePointCategoryFacts {
    pub is_space_separator: bool,
    pub is_punctuation: bool,
    pub is_letter: bool,
    pub is_number: bool,
    pub is_symbol: bool,
    pub is_open_punctuation: bool,
    pub is_dash_punctuation: bool,
}

unsafe extern "C" {
    fn ladybird_layout_code_point_category_facts(code_point: u32) -> FfiCodePointCategoryFacts;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiAnonymousTableBoxKind {
    TableRow,
    TableCell,
    Table,
    InlineTable,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FfiInsertionMode {
    Append,
    Prepend,
    InDomOrder,
}

#[repr(C)]
pub struct FfiTreeBuilderCallbacks {
    pub context: *mut c_void,
    pub take_fieldset_overflow_for_content_wrapper:
        unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiAnonymousStyleOverrides,
    pub prepare_subtree_for_detach: unsafe extern "C" fn(*mut c_void, *mut c_void),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TraversalDecision {
    Continue,
    SkipChildrenAndContinue,
    Break,
}

struct TreeBuilderHost<'a> {
    callbacks: &'a FfiTreeBuilderCallbacks,
    arena: *mut LayoutNodeArena,
}

fn node_has_flag(data: &NodeData, flag: NodeFlag) -> bool {
    data.flags.get() & flag as u32 != 0
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct FfiNodeKindFacts {
    pub is_box: bool,
    pub is_block_container: bool,
    pub is_text: bool,
    pub is_svg_box: bool,
    pub is_replaced_box: bool,
}

// Exhaustive on purpose: adding a NodeKind variant must not compile until its facts are declared.
fn kind_facts(kind: NodeKind) -> FfiNodeKindFacts {
    const NON_BOX: FfiNodeKindFacts = FfiNodeKindFacts {
        is_box: false,
        is_block_container: false,
        is_text: false,
        is_svg_box: false,
        is_replaced_box: false,
    };
    const TEXT: FfiNodeKindFacts = FfiNodeKindFacts {
        is_text: true,
        ..NON_BOX
    };
    const BOX: FfiNodeKindFacts = FfiNodeKindFacts {
        is_box: true,
        ..NON_BOX
    };
    const REPLACED_BOX: FfiNodeKindFacts = FfiNodeKindFacts {
        is_replaced_box: true,
        ..BOX
    };
    const BLOCK_CONTAINER: FfiNodeKindFacts = FfiNodeKindFacts {
        is_block_container: true,
        ..BOX
    };
    const SVG_BOX: FfiNodeKindFacts = FfiNodeKindFacts {
        is_svg_box: true,
        ..BOX
    };

    match kind {
        NodeKind::Unset | NodeKind::BreakNode | NodeKind::InlineNode | NodeKind::Node | NodeKind::NodeWithStyle => {
            NON_BOX
        }
        NodeKind::GeneratedTextNode | NodeKind::TextNode => TEXT,
        NodeKind::Box | NodeKind::ListItemMarkerBox => BOX,
        NodeKind::AudioBox
        | NodeKind::CanvasBox
        | NodeKind::CheckBox
        | NodeKind::ImageBox
        | NodeKind::NavigableContainerViewport
        | NodeKind::RadioButton
        | NodeKind::ReplacedBox
        | NodeKind::SVGSVGBox
        | NodeKind::VideoBox => REPLACED_BOX,
        NodeKind::BlockContainer
        | NodeKind::FieldSetBox
        | NodeKind::LegendBox
        | NodeKind::ListItemBox
        | NodeKind::RangeInputBox
        | NodeKind::SVGForeignObjectBox
        | NodeKind::TableWrapper
        | NodeKind::TextAreaBox
        | NodeKind::TextInputBox
        | NodeKind::Viewport => BLOCK_CONTAINER,
        NodeKind::SVGBox
        | NodeKind::SVGClipBox
        | NodeKind::SVGGeometryBox
        | NodeKind::SVGGraphicsBox
        | NodeKind::SVGImageBox
        | NodeKind::SVGMaskBox
        | NodeKind::SVGPatternBox
        | NodeKind::SVGTextBox
        | NodeKind::SVGTextPathBox => SVG_BOX,
    }
}

fn node_kind_is_box(kind: NodeKind) -> bool {
    kind_facts(kind).is_box
}

fn node_kind_is_block_container(kind: NodeKind) -> bool {
    kind_facts(kind).is_block_container
}

fn node_kind_is_text(kind: NodeKind) -> bool {
    kind_facts(kind).is_text
}

fn node_kind_is_svg_box(kind: NodeKind) -> bool {
    kind_facts(kind).is_svg_box
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_node_kind_is_replaced_box(kind: NodeKind) -> bool {
    node_facts::kind_is_replaced_box(kind)
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_node_kind_is_svg_box(kind: NodeKind) -> bool {
    node_facts::kind_is_svg_box(kind)
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_node_kind_is_svg_graphics_box(kind: NodeKind) -> bool {
    svg_formatting_context::kind_is_svg_graphics_box(kind)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_is_atomic_inline(arena: *mut c_void, id: NodeSlotId) -> bool {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    let arena = unsafe { LayoutNodeArena::from_handle(arena) };
    let data = arena.data(id);
    node_facts::node_is_atomic_inline(data, node_facts::node_style_view(data))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_is_fragmented_inline(arena: *mut c_void, id: NodeSlotId) -> bool {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    let arena = unsafe { LayoutNodeArena::from_handle(arena) };
    let data = arena.data(id);
    node_facts::node_is_fragmented_inline(data, node_facts::node_style_view(data))
}

fn node_is_generated_for_pseudo_element(data: &NodeData) -> bool {
    data.generated_for.get() != 0
}

fn node_is_inline_outside(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    node_kind_is_text(host.data(node).kind.get())
        || host
            .style(node)
            .is_some_and(|style| style.display().is_inline_outside())
}

fn node_is_out_of_flow(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    node_facts::node_is_out_of_flow(host.data(node), host.style(node))
}

fn node_has_replaced_element_table_display_adjustment(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    node_has_flag(host.data(node), NodeFlag::IsReplacedElement)
        && host.style(node).is_some_and(|style| {
            let display = style.display_before_box_type_transformation();
            display.is_table_inside() || display.is_internal_table() || display.is_table_caption()
        })
}

fn node_is_fragmented_inline(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    let data = host.data(node);
    node_facts::node_is_fragmented_inline(data, host.style(node))
}

impl TreeBuilderHost<'_> {
    fn data(&self, node: LayoutNode) -> &NodeData {
        assert!(!node.is_invalid());
        // SAFETY: Entry points guarantee that the arena remains live, and callers only retain the reference until the
        // next mutation callback.
        unsafe { &*self.arena }.data(node)
    }

    fn style(&self, node: LayoutNode) -> Option<ComputedValuesView<'_>> {
        assert!(!node.is_invalid());
        // SAFETY: Entry points guarantee that the arena remains live, and callers only retain the reader until the
        // next mutation callback.
        unsafe { (*self.arena).style_payloads(node) }.map(|payloads| ComputedValuesView::new(&payloads.groups))
    }

    fn display(&self, node: LayoutNode) -> FfiDisplay {
        self.style(node).map_or_else(FfiDisplay::block, |style| style.display())
    }

    fn display_before_box_type_transformation(&self, node: LayoutNode) -> FfiDisplay {
        self.style(node).map_or_else(FfiDisplay::block, |style| {
            style.display_before_box_type_transformation()
        })
    }

    fn shell(&self, node: LayoutNode) -> *mut c_void {
        let shell = self.arena().node_shell(node);
        assert!(!shell.is_null());
        shell
    }

    fn set_children_are_inline(&self, node: LayoutNode, children_are_inline: bool) {
        self.arena()
            .set_node_flag(node, NodeFlag::ChildrenAreInline, children_are_inline);
    }

    fn arena(&self) -> &LayoutNodeArena {
        // SAFETY: Entry points guarantee that the arena remains live.
        unsafe { &*self.arena }
    }

    fn created(&self, slot: NodeSlotId) -> UnplacedLayoutNode {
        UnplacedLayoutNode::new(slot)
    }

    fn create_anonymous_box(
        &self,
        parent: LayoutNode,
        style_kind: FfiAnonymousStyleKind,
        overrides: FfiAnonymousStyleOverrides,
        node_kind: NodeKind,
    ) -> UnplacedLayoutNode {
        self.create_anonymous_box_from_style_record(
            self.arena().node_style_record(parent),
            style_kind,
            overrides,
            node_kind,
        )
    }

    fn create_anonymous_box_from_style_record(
        &self,
        parent_style_record: u64,
        style_kind: FfiAnonymousStyleKind,
        overrides: FfiAnonymousStyleOverrides,
        node_kind: NodeKind,
    ) -> UnplacedLayoutNode {
        let derived = self
            .arena()
            .derive_anonymous_style_record(parent_style_record, style_kind, overrides);
        // SAFETY: Entry points guarantee that the arena remains live, and callers hold no reference derived from
        // it across the allocation.
        let slot = unsafe { &mut *self.arena }.allocate_unbound(std::ptr::null_mut());
        self.arena().stamp_anonymous_box(slot, node_kind, derived);
        self.arena().refresh_insets_use_anchor_functions_flag(slot);
        if node_kind == NodeKind::InlineNode {
            assert!(!self.arena().node_shell(slot).is_null());
        }
        UnplacedLayoutNode::new(slot)
    }

    fn anonymous_wrapper_overrides(&self, parent: LayoutNode) -> FfiAnonymousStyleOverrides {
        FfiAnonymousStyleOverrides {
            inline_block_wrapper: self.display(parent).is_inline_block() && self.first_child(parent).is_invalid(),
            ..FfiAnonymousStyleOverrides::default()
        }
    }

    fn create_anonymous_wrapper_box(&self, parent: LayoutNode) -> UnplacedLayoutNode {
        self.create_anonymous_box(
            parent,
            FfiAnonymousStyleKind::Wrapper,
            self.anonymous_wrapper_overrides(parent),
            NodeKind::BlockContainer,
        )
    }

    fn attach_child(&self, parent: LayoutNode, child: UnplacedLayoutNode, before: LayoutNode) {
        self.arena().attach_child(parent, child, before);
    }

    fn move_child(&self, child: LayoutNode, new_parent: LayoutNode, before: LayoutNode) {
        self.arena().move_child(child, new_parent, before);
    }

    fn free_unplaced(&self, node: UnplacedLayoutNode) {
        self.free_subtree(node.into_slot());
    }

    fn free_subtree(&self, node: LayoutNode) {
        free_subtree_and_destroy_shells(self.arena, node);
    }

    fn parent(&self, node: LayoutNode) -> LayoutNode {
        self.data(node).parent.get()
    }

    fn first_child(&self, node: LayoutNode) -> LayoutNode {
        self.data(node).first_child.get()
    }

    fn next_sibling(&self, node: LayoutNode) -> LayoutNode {
        self.data(node).next_sibling.get()
    }

    fn previous_sibling(&self, node: LayoutNode) -> LayoutNode {
        self.data(node).previous_sibling.get()
    }

    fn last_child(&self, node: LayoutNode) -> LayoutNode {
        self.data(node).last_child.get()
    }

    fn for_each_in_inclusive_subtree(
        &self,
        root: LayoutNode,
        mut callback: impl FnMut(LayoutNode) -> TraversalDecision,
    ) {
        let mut current = root;
        while !current.is_invalid() {
            let decision = callback(current);
            if decision == TraversalDecision::Break {
                return;
            }

            if decision != TraversalDecision::SkipChildrenAndContinue {
                let first_child = self.first_child(current);
                if !first_child.is_invalid() {
                    current = first_child;
                    continue;
                }
            }
            if current == root {
                break;
            }

            let next_sibling = self.next_sibling(current);
            if !next_sibling.is_invalid() {
                current = next_sibling;
                continue;
            }

            while current != root && self.next_sibling(current).is_invalid() {
                current = self.parent(current);
            }
            if current == root {
                break;
            }

            current = self.next_sibling(current);
        }
    }

    fn remove_nodes(&self, nodes: &[LayoutNode]) {
        for &node in nodes {
            let parent = self.parent(node);
            assert!(!parent.is_invalid());
            self.arena().detach_child(parent, node);
        }
        for &node in nodes {
            self.free_subtree(node);
        }
    }

    fn wrap_in_anonymous(&self, nodes: &[LayoutNode], nearest_sibling: LayoutNode, kind: FfiAnonymousTableBoxKind) {
        assert!(!nodes.is_empty());
        let parent = self.parent(nodes[0]);
        assert!(!parent.is_invalid());
        let (style_kind, node_kind) = match kind {
            FfiAnonymousTableBoxKind::TableRow => (FfiAnonymousStyleKind::TableRow, NodeKind::Box),
            FfiAnonymousTableBoxKind::TableCell => (FfiAnonymousStyleKind::TableCell, NodeKind::BlockContainer),
            FfiAnonymousTableBoxKind::Table => (FfiAnonymousStyleKind::Table, NodeKind::Box),
            FfiAnonymousTableBoxKind::InlineTable => (FfiAnonymousStyleKind::InlineTable, NodeKind::Box),
        };
        let wrapper = self.create_anonymous_box(parent, style_kind, FfiAnonymousStyleOverrides::default(), node_kind);
        let wrapper_slot = wrapper.slot();
        for &node in nodes {
            self.move_child(node, wrapper_slot, NodeSlotId::INVALID);
        }
        let parent_children_are_inline = node_has_flag(self.data(parent), NodeFlag::ChildrenAreInline);
        self.set_children_are_inline(wrapper_slot, parent_children_are_inline);
        self.attach_child(parent, wrapper, nearest_sibling);
    }
}

impl table_formatting_context::TableTree for TreeBuilderHost<'_> {
    fn first_child(&self, node: LayoutNode) -> LayoutNode {
        TreeBuilderHost::first_child(self, node)
    }

    fn next_sibling(&self, node: LayoutNode) -> LayoutNode {
        TreeBuilderHost::next_sibling(self, node)
    }

    fn node_data(&self, node: LayoutNode) -> &NodeData {
        self.data(node)
    }

    fn display(&self, node: LayoutNode) -> FfiDisplay {
        TreeBuilderHost::display(self, node)
    }
}

fn is_inclusive_layout_ancestor_of(host: &TreeBuilderHost<'_>, ancestor: LayoutNode, node: LayoutNode) -> bool {
    let mut current = node;
    while !current.is_invalid() {
        if current == ancestor {
            return true;
        }
        current = host.parent(current);
    }
    false
}

// Restructuring the tree at a node outside the subtree being rebuilt in place means the update
// escaped every rebuild root, so partial relayout is no longer sound for this build.
fn note_layout_tree_restructuring_at(host: &TreeBuilderHost<'_>, state: &mut TreeBuilderState, node: LayoutNode) {
    if state.current_rebuild_root.is_invalid() {
        return;
    }
    if !is_inclusive_layout_ancestor_of(host, state.current_rebuild_root, node) {
        state.layout_tree_update_escaped_rebuild_roots = true;
    }
}

fn has_inline_or_in_flow_block_children(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    let mut child = host.first_child(node);
    while !child.is_invalid() {
        if node_is_inline_outside(host, child) || !node_is_out_of_flow(host, child) {
            return true;
        }
        child = host.next_sibling(child);
    }
    false
}

fn has_in_flow_block_children(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    if node_has_flag(host.data(node), NodeFlag::ChildrenAreInline) {
        return false;
    }
    let mut child = host.first_child(node);
    while !child.is_invalid() {
        if !node_is_inline_outside(host, child) && !node_is_out_of_flow(host, child) {
            return true;
        }
        child = host.next_sibling(child);
    }
    false
}

fn is_out_of_flow_table_internal_child_of_table_root(
    host: &TreeBuilderHost<'_>,
    parent: LayoutNode,
    child: LayoutNode,
) -> bool {
    let child_data = host.data(child);
    host.display(parent).is_table_inside()
        && node_has_flag(child_data, NodeFlag::HasStyle)
        && !node_has_flag(child_data, NodeFlag::Anonymous)
        && node_is_out_of_flow(host, child)
        && !node_has_replaced_element_table_display_adjustment(host, child)
        && is_table_non_root_box_with_display(host.display_before_box_type_transformation(child))
}

fn create_anonymous_wrapper(host: &TreeBuilderHost<'_>, parent: LayoutNode) -> LayoutNode {
    let wrapper = host.create_anonymous_wrapper_box(parent);
    let wrapper_slot = wrapper.slot();
    host.attach_child(parent, wrapper, NodeSlotId::INVALID);
    wrapper_slot
}

fn last_child_creating_anonymous_wrapper_if_needed(host: &TreeBuilderHost<'_>, parent: LayoutNode) -> LayoutNode {
    let last_child = host.last_child(parent);
    if last_child.is_invalid() {
        return create_anonymous_wrapper(host, parent);
    }
    let data = host.data(last_child);
    if !node_has_flag(data, NodeFlag::Anonymous)
        || !node_has_flag(data, NodeFlag::ChildrenAreInline)
        || node_is_generated_for_pseudo_element(data)
    {
        return create_anonymous_wrapper(host, parent);
    }
    last_child
}

// The insertion_parent_for_*() functions maintain the invariant that the in-flow children of
// block-level boxes must be either all block-level or all inline-level.
fn insertion_parent_for_inline_node(host: &TreeBuilderHost<'_>, parent: LayoutNode) -> LayoutNode {
    let data = host.data(parent);
    if matches!(data.kind.get(), NodeKind::FieldSetBox | NodeKind::SVGForeignObjectBox) {
        return last_child_creating_anonymous_wrapper_if_needed(host, parent);
    }

    // SVG layout ignores the inline/block distinction, and an anonymous wrapper would only hide
    // the child from SVGFormattingContext (e.g. a shape with a foreignObject sibling).
    if node_kind_is_svg_box(data.kind.get()) || data.kind.get() == NodeKind::SVGSVGBox {
        return parent;
    }

    let parent_display = host.style(parent).map(|style| style.display());
    if node_is_inline_outside(host, parent) && parent_display.is_some_and(|display| display.is_flow_inside()) {
        return parent;
    }

    if parent_display.is_some_and(|display| display.is_flex_inside() || display.is_grid_inside()) {
        return last_child_creating_anonymous_wrapper_if_needed(host, parent);
    }

    if !has_in_flow_block_children(host, parent) || node_has_flag(data, NodeFlag::ChildrenAreInline) {
        return parent;
    }

    // Parent has block-level children, insert into an anonymous wrapper block (and create it first if needed)
    last_child_creating_anonymous_wrapper_if_needed(host, parent)
}

fn nearest_rebuildable_container(host: &TreeBuilderHost<'_>, node: LayoutNode) -> LayoutNode {
    let mut container = node;
    loop {
        let data = host.data(container);
        if !node_has_flag(data, NodeFlag::Anonymous) && data.kind.get() != NodeKind::InlineNode {
            return container;
        }
        container = host.parent(container);
        assert!(!container.is_invalid());
    }
}

fn insertion_parent_for_block_node(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    parent: LayoutNode,
    node: LayoutNode,
    mode: FfiInsertionMode,
) -> LayoutNode {
    let layout = host.layout();
    let parent_data = layout.data(parent);

    // Inline is fine for in-flow block children (interrupting blocks) and for out-of-flow children;
    // the inline formatting context emits items for both.
    if !node_has_flag(layout.data(node), NodeFlag::Anonymous)
        && node_is_inline_outside(&layout, parent)
        && layout
            .style(parent)
            .is_some_and(|style| style.display().is_flow_inside())
    {
        return parent;
    }

    // SVG layout ignores the inline/block distinction; wrapping existing inline-level siblings
    // (e.g. shapes next to a foreignObject) would only hide them from SVGFormattingContext.
    if node_kind_is_svg_box(parent_data.kind.get()) || parent_data.kind.get() == NodeKind::SVGSVGBox {
        return parent;
    }

    // Make sure we're not inserting into an inline node, since those do not support block nodes.
    let mut new_parent = parent;
    while layout.data(new_parent).kind.get() == NodeKind::InlineNode {
        new_parent = layout.parent(new_parent);
        assert!(!new_parent.is_invalid());
    }

    if new_parent != parent && !is_inclusive_layout_ancestor_of(&layout, state.new_subtree_root, new_parent) {
        let container = nearest_rebuildable_container(&layout, new_parent);
        // SAFETY: `container` is a live, attached layout node.
        let element = unsafe { (host.callbacks.layout_node_dom_element)(layout.shell(container)) };
        if !state.layout_tree_rebuild_requests.contains(&element) {
            state.layout_tree_rebuild_requests.push(element);
        }
    }

    // If the parent block has no children, insert this block into parent.
    if !has_inline_or_in_flow_block_children(&layout, new_parent) {
        return new_parent;
    }

    // Table-internal boxes may have been blockified before insertion, but table fixup still needs to see them as
    // direct table children instead of grouping them with neighboring table whitespace.
    if is_out_of_flow_table_internal_child_of_table_root(&layout, new_parent, node) {
        return new_parent;
    }

    let new_parent_data = layout.data(new_parent);

    // If the block is out-of-flow,
    if node_is_out_of_flow(&layout, node) {
        let last_child = layout.last_child(new_parent);
        assert!(!last_child.is_invalid());
        let last_child_data = layout.data(last_child);

        // And we're appending while the parent's last child is an anonymous block, join that
        // anonymous block. Prepended boxes (e.g. an absolutely positioned ::before) belong at the
        // very start of the parent, not at the start of its trailing inline run.
        let new_parent_display = layout.style(new_parent).map(|style| style.display());
        if mode == FfiInsertionMode::Append
            && !new_parent_display.is_some_and(|display| display.is_flex_inside() || display.is_grid_inside())
            && !node_is_generated_for_pseudo_element(last_child_data)
            && node_has_flag(last_child_data, NodeFlag::Anonymous)
            && node_has_flag(last_child_data, NodeFlag::ChildrenAreInline)
        {
            return last_child;
        }

        // Otherwise, insert this block into parent.
        return new_parent;
    }

    // If the parent block has block-level children, insert this block into parent.
    if !node_has_flag(new_parent_data, NodeFlag::ChildrenAreInline) {
        return new_parent;
    }

    // Parent block has inline-level children (our siblings); wrap these siblings into an anonymous wrapper block.
    note_layout_tree_restructuring_at(&layout, state, new_parent);
    let mut children_to_wrap = Vec::new();
    let mut child = layout.first_child(new_parent);
    while !child.is_invalid() {
        if !is_out_of_flow_table_internal_child_of_table_root(&layout, new_parent, child) {
            children_to_wrap.push(child);
        }
        child = layout.next_sibling(child);
    }
    let wrapper = layout.create_anonymous_wrapper_box(new_parent);
    let wrapper_slot = wrapper.slot();
    layout.set_children_are_inline(wrapper_slot, true);
    for child in children_to_wrap {
        layout.move_child(child, wrapper_slot, NodeSlotId::INVALID);
    }
    layout.set_children_are_inline(new_parent, false);
    layout.attach_child(new_parent, wrapper, NodeSlotId::INVALID);

    // Then it's safe to insert this block into parent.
    new_parent
}

fn insert_child_in_dom_order(
    host: &DomTreeBuilderHost<'_>,
    parent: LayoutNode,
    child: UnplacedLayoutNode,
    dom_node: *mut c_void,
) {
    assert!(!dom_node.is_null());
    let layout = host.layout();

    // An inline child of a block container with block children is placed in a newly appended
    // anonymous wrapper. Move that empty wrapper to the child's DOM position before filling it.
    let (parent_is_empty_anonymous_wrapper, wrapper_parent) = {
        let data = layout.data(parent);
        (
            node_has_flag(data, NodeFlag::Anonymous) && data.first_child.get().is_invalid(),
            data.parent.get(),
        )
    };
    if parent_is_empty_anonymous_wrapper && !wrapper_parent.is_invalid() {
        let mut sibling = host.next_sibling(dom_node);
        while !sibling.is_null() {
            let mut sibling_layout_node = host.dom_node_layout_node(sibling);
            while !sibling_layout_node.is_invalid() && layout.parent(sibling_layout_node) != wrapper_parent {
                sibling_layout_node = layout.parent(sibling_layout_node);
            }
            if !sibling_layout_node.is_invalid() && sibling_layout_node != parent {
                layout.move_child(parent, wrapper_parent, sibling_layout_node);
                break;
            }
            sibling = host.next_sibling(sibling);
        }
    }

    let mut sibling = host.next_sibling(dom_node);
    while !sibling.is_null() {
        let sibling_layout_node = host.dom_node_layout_node(sibling);
        if !sibling_layout_node.is_invalid() && layout.parent(sibling_layout_node) == parent {
            layout.attach_child(parent, child, sibling_layout_node);
            return;
        }
        sibling = host.next_sibling(sibling);
    }

    // SAFETY: `parent` is a live layout node.
    let parent_element = unsafe { (host.callbacks.layout_node_dom_element)(layout.shell(parent)) };
    if !parent_element.is_null() {
        // SAFETY: `parent_element` is a live element.
        let after_layout_node =
            unsafe { (host.callbacks.element_pseudo_layout_node)(parent_element, FfiPseudoElement::After) };
        if !after_layout_node.is_invalid() {
            let mut after_layout_child = after_layout_node;
            while !layout.parent(after_layout_child).is_invalid() && layout.parent(after_layout_child) != parent {
                after_layout_child = layout.parent(after_layout_child);
            }
            if layout.parent(after_layout_child) == parent {
                layout.attach_child(parent, child, after_layout_child);
                return;
            }
        }
    }

    let mut layout_child = layout.first_child(parent);
    while !layout_child.is_invalid() {
        if layout.data(layout_child).generated_for.get() == GENERATED_FOR_AFTER {
            layout.attach_child(parent, child, layout_child);
            return;
        }
        layout_child = layout.next_sibling(layout_child);
    }
    layout.attach_child(parent, child, NodeSlotId::INVALID);
}

fn insert_node_into_inline_or_block_ancestor(
    host: &DomTreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    nearest_insertion_ancestor: LayoutNode,
    node: UnplacedLayoutNode,
    is_inline_outside: bool,
    mode: FfiInsertionMode,
    dom_node: *mut c_void,
) {
    assert!(!nearest_insertion_ancestor.is_invalid());
    let layout = host.layout();
    let node_slot = node.slot();

    let insertion_point = if is_inline_outside {
        insertion_parent_for_inline_node(&layout, nearest_insertion_ancestor)
    } else {
        insertion_parent_for_block_node(host, state, nearest_insertion_ancestor, node_slot, mode)
    };

    // Insertion parents can be above the subtree being rebuilt in place: inline ancestors are
    // skipped, and out-of-flow boxes can join a trailing anonymous sibling. InDomOrder is only
    // selected after proving that an inline box can be added directly to a retained parent, so
    // that parent insertion is the planned update rather than an escape from its new subtree.
    if mode != FfiInsertionMode::InDomOrder {
        note_layout_tree_restructuring_at(&layout, state, insertion_point);
    }
    match mode {
        FfiInsertionMode::Append => layout.attach_child(insertion_point, node, NodeSlotId::INVALID),
        FfiInsertionMode::Prepend => {
            let first_child = layout.first_child(insertion_point);
            layout.attach_child(insertion_point, node, first_child);
        }
        FfiInsertionMode::InDomOrder => insert_child_in_dom_order(host, insertion_point, node, dom_node),
    }

    if is_inline_outside {
        // After inserting an inline-level box into a parent, mark the parent as having inline children.
        layout.set_children_are_inline(insertion_point, true);
    } else if !node_is_out_of_flow(&layout, node_slot) {
        // Inline-flow parents keep their inline children flag; their IFC may contain interrupting blocks.
        if !node_is_inline_outside(&layout, insertion_point)
            || !layout
                .style(insertion_point)
                .is_some_and(|style| style.display().is_flow_inside())
        {
            layout.set_children_are_inline(insertion_point, false);
        }
    }
}

// https://drafts.csswg.org/css-pseudo-4/#first-letter-pattern
pub(crate) fn find_first_letter_in_text(
    text: &[u16],
    preserves_segment_breaks: bool,
    next_grapheme_boundary: impl Fn(usize) -> usize,
    code_point_facts: impl Fn(u32) -> FfiCodePointCategoryFacts,
) -> FfiFirstLetterTarget {
    // NB: Matches the first-letter text pattern: (P (Zs|P)*)? (L|N|S) ((Zs|P-(Ps|Pd))* (P-(Ps|Pd))?)?

    let code_units = text.len();
    let mut match_start = 0;
    while match_start < code_units {
        let mut cursor = match_start;
        let starting_code_point = code_point_at(text, cursor);

        // When white-space preserves segment breaks, a newline before any letter puts the letter on a later line, so
        // the first formatted line is empty and ::first-letter must not match.
        if preserves_segment_breaks && (starting_code_point == b'\n' as u32 || starting_code_point == b'\r' as u32) {
            return FfiFirstLetterTarget::not_found();
        }

        let starting_facts = code_point_facts(starting_code_point);

        // A valid match starts with either a P, or the letter itself.
        let has_preceding = starting_facts.is_punctuation;
        if !(has_preceding || starting_facts.is_letter || starting_facts.is_number || starting_facts.is_symbol) {
            match_start += code_unit_length_for_code_point(starting_code_point);
            continue;
        }

        if has_preceding {
            // Preceding group: P followed by (Zs|P)*.
            cursor = next_grapheme_boundary(cursor);
            while cursor < code_units {
                let code_point = code_point_at(text, cursor);
                let facts = code_point_facts(code_point);
                // For the preceding run: Zs excluding U+3000 IDEOGRAPHIC SPACE.
                let is_preceding_intervening_space = code_point != 0x3000 && facts.is_space_separator;
                if !facts.is_punctuation && !is_preceding_intervening_space {
                    break;
                }
                cursor = next_grapheme_boundary(cursor);
            }
        }

        // The letter (L|N|S) must follow the preceding group. If the preceding punctuation consumed the entire text
        // node, accept it as the first-letter.
        if cursor >= code_units {
            return FfiFirstLetterTarget {
                text_node: std::ptr::null_mut(),
                text_layout_node: NodeSlotId::INVALID,
                letter_start: match_start,
                letter_end: cursor,
                source_length: code_units,
                found: true,
            };
        }
        let letter_facts = code_point_facts(code_point_at(text, cursor));
        if !(letter_facts.is_letter || letter_facts.is_number || letter_facts.is_symbol) {
            match_start += code_unit_length_for_code_point(starting_code_point);
            continue;
        }

        let mut letter_end = next_grapheme_boundary(cursor);

        // Trailing group: greedy match of (Zs|P-(Ps|Pd))*.
        while letter_end < code_units {
            let code_point = code_point_at(text, letter_end);
            let facts = code_point_facts(code_point);
            // For the trailing run: Zs excluding U+3000 IDEOGRAPHIC SPACE and word separators.
            // NB: css-text-4 defines word separators as a non-exhaustive list, but of the seven code
            //     points it names only U+0020 SPACE and U+00A0 NO-BREAK SPACE are in the Zs category;
            //     the rest are in Po and would never reach this check. Fixed-width spaces are explicitly
            //     not word separators per the spec's note, so they remain valid intervening Zs here.
            let is_trailing_intervening_space =
                !matches!(code_point, 0x0020 | 0x00a0 | 0x3000) && facts.is_space_separator;
            // NB: The css-pseudo specification excludes Ps and Pd classes (opening punctuation and dashes) from the
            //     trailing run, whereas CSS 2.1 allowed all classes in both the preceding and trailing runs.
            let is_trailing_punctuation =
                facts.is_punctuation && !facts.is_open_punctuation && !facts.is_dash_punctuation;
            if !is_trailing_intervening_space && !is_trailing_punctuation {
                break;
            }
            letter_end = next_grapheme_boundary(letter_end);
        }

        return FfiFirstLetterTarget {
            text_node: std::ptr::null_mut(),
            text_layout_node: NodeSlotId::INVALID,
            letter_start: match_start,
            letter_end,
            source_length: code_units,
            found: true,
        };
    }
    FfiFirstLetterTarget::not_found()
}

fn find_first_letter_in_layout_text(host: &TreeBuilderHost<'_>, node: LayoutNode) -> FfiFirstLetterTarget {
    // SAFETY: Tree building owns the arena, and no borrow crosses the source callback.
    let source = unsafe { super::rendered_text::text_source_for_node(host.arena, node) };
    // SAFETY: Copy the raw source before any further host call. First-letter
    // matching determines source ranges before text transforms are applied.
    let text = unsafe { source.text.to_utf16() }.expect("first-letter source carries no storage");
    let segmenter = GraphemeSegmenter::new(&text);
    let preserves_segment_breaks = matches!(
        host.style(host.parent(node))
            .expect("text parent has style")
            .inherited_text()
            .white_space_collapse,
        white_space_collapse::PRESERVE | white_space_collapse::PRESERVE_BREAKS | white_space_collapse::BREAK_SPACES
    );
    let mut target = find_first_letter_in_text(
        &text,
        preserves_segment_breaks,
        |index| segmenter.next_boundary(index, false).unwrap_or(text.len()),
        |code_point| {
            // SAFETY: This service classifies a scalar value without accessing layout.
            unsafe { ladybird_layout_code_point_category_facts(code_point) }
        },
    );
    if target.found {
        target.text_node = host.shell(node);
        target.text_layout_node = node;
    }
    target
}

fn create_first_letter_boxes(host: &DomTreeBuilderHost<'_>, element: *mut c_void, target: FfiFirstLetterTarget) {
    let layout_host = host.layout();
    // SAFETY: `element` is a live Element and `target` identifies a live descendant text node.
    let nodes = unsafe { (host.callbacks.create_first_letter_nodes)(host.callbacks.builder, element, target) };
    let first_letter_slice = layout_host.created(nodes.first_letter_slice);
    let remainder_slice = layout_host.created(nodes.remainder_slice);
    if nodes.wrapper.is_invalid() {
        layout_host.free_unplaced(first_letter_slice);
        layout_host.free_unplaced(remainder_slice);
        return;
    }
    if layout_host.data(nodes.remainder_slice).kind.get() == NodeKind::TextNode {
        // SAFETY: The host callback has returned and no arena borrow survives it.
        // Initialize the source ranges before attaching or rendering either slice.
        unsafe { &mut *layout_host.arena }.set_first_letter_slices(
            nodes.first_letter_slice,
            nodes.remainder_slice,
            target.letter_end,
            target.source_length,
        );
    }
    let wrapper = layout_host.created(nodes.wrapper);
    let wrapper_slot = wrapper.slot();
    let text_node = target.text_layout_node;
    let parent = layout_host.parent(text_node);
    assert!(!parent.is_invalid());
    layout_host.set_children_are_inline(wrapper_slot, true);
    layout_host.attach_child(wrapper_slot, first_letter_slice, NodeSlotId::INVALID);
    layout_host.attach_child(parent, wrapper, text_node);
    layout_host.attach_child(parent, remainder_slice, text_node);
    layout_host.arena().detach_child(parent, text_node);
    layout_host.free_subtree(text_node);
}

fn is_marker_content(data: &NodeData) -> bool {
    data.kind.get() == NodeKind::ListItemMarkerBox || data.generated_for.get() == GENERATED_FOR_MARKER
}

// https://drafts.csswg.org/css-pseudo-4/#first-letter-application
fn find_first_letter_in_block(host: &DomTreeBuilderHost<'_>, block: LayoutNode) -> FfiFirstLetterTarget {
    let layout_host = host.layout();
    // NB: This walks a block container's inline descendants looking for the first-letter text. If the block has block
    //     children instead of inline, recurses into each in-flow block child in turn.
    if node_has_flag(layout_host.data(block), NodeFlag::ChildrenAreInline) {
        let mut result = FfiFirstLetterTarget::not_found();
        let mut is_root = true;
        layout_host.for_each_in_inclusive_subtree(block, |node| {
            if is_root {
                is_root = false;
                return TraversalDecision::Continue;
            }
            let data = layout_host.data(node);
            if is_marker_content(data) || node_is_out_of_flow(&layout_host, node) {
                return TraversalDecision::SkipChildrenAndContinue;
            }
            if node_kind_is_text(data.kind.get()) {
                result = find_first_letter_in_layout_text(&layout_host, node);
                return if result.found {
                    TraversalDecision::Break
                } else {
                    TraversalDecision::Continue
                };
            }
            if node_is_fragmented_inline(&layout_host, node) {
                return TraversalDecision::Continue;
            }
            TraversalDecision::Break
        });
        return result;
    }

    // We have no inline content of our own but ::first-letter can still apply to text in an in-flow block descendant,
    // so walk into each in-flow block child in document order until one yields a letter.
    let mut child = layout_host.first_child(block);
    while !child.is_invalid() {
        let data = layout_host.data(child);
        let is_anonymous = node_has_flag(data, NodeFlag::Anonymous);
        if is_marker_content(data) || node_is_out_of_flow(&layout_host, child) {
            child = layout_host.next_sibling(child);
            continue;
        }
        if !node_kind_is_block_container(data.kind.get()) {
            break;
        }
        // Stop descending if this child block defines its own ::first-letter: the child will style the first letter
        // inside it, so the ancestor's ::first-letter must not also claim the same letter.
        // SAFETY: The child shell and its associated DOM node remain live throughout the walk.
        if !is_anonymous && unsafe { (host.callbacks.layout_node_has_first_letter_style)(layout_host.shell(child)) } {
            break;
        }
        let target = find_first_letter_in_block(host, child);
        if target.found {
            return target;
        }
        if !is_anonymous {
            break;
        }
        child = layout_host.next_sibling(child);
    }
    FfiFirstLetterTarget::not_found()
}

fn wrap_button_contents_if_needed(host: &TreeBuilderHost<'_>, layout_node: LayoutNode) {
    assert!(!layout_node.is_invalid());
    if !node_has_flag(host.data(layout_node), NodeFlag::UsesButtonLayout) {
        return;
    }

    // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
    // If the element is an input element, or if it is a button element and its computed value for 'display' is not
    // 'inline-grid', 'grid', 'inline-flex', or 'flex', then the element's box has a child anonymous button content
    // box with the following behaviors:
    let display = host.style(layout_node).map(|style| style.display());
    if !display.is_some_and(|display| display.is_grid_inside() || display.is_flex_inside()) {
        let children_are_inline = node_has_flag(host.data(layout_node), NodeFlag::ChildrenAreInline);
        let mut children = Vec::new();
        let mut child = host.first_child(layout_node);
        while !child.is_invalid() {
            children.push(child);
            child = host.next_sibling(child);
        }

        let flex_wrapper = host.create_anonymous_box(
            layout_node,
            FfiAnonymousStyleKind::ButtonFlexWrapper,
            FfiAnonymousStyleOverrides::default(),
            NodeKind::BlockContainer,
        );
        let content_box = host.create_anonymous_box(
            layout_node,
            FfiAnonymousStyleKind::ButtonContentBox,
            host.anonymous_wrapper_overrides(layout_node),
            NodeKind::BlockContainer,
        );
        let flex_wrapper_slot = flex_wrapper.slot();
        let content_box_slot = content_box.slot();
        host.attach_child(flex_wrapper_slot, content_box, NodeSlotId::INVALID);
        host.attach_child(layout_node, flex_wrapper, NodeSlotId::INVALID);
        host.set_children_are_inline(content_box_slot, children_are_inline);
        for child in children {
            host.move_child(child, content_box_slot, NodeSlotId::INVALID);
        }
        host.set_children_are_inline(layout_node, false);
    }
}

// https://html.spec.whatwg.org/multipage/rendering.html#rendered-legend
// The rendered legend is the first legend child whose used 'float' is 'none' and whose used
// 'position' is neither 'absolute' nor 'fixed'.
fn rendered_legend(host: &TreeBuilderHost<'_>, fieldset: LayoutNode) -> LayoutNode {
    let mut child = host.first_child(fieldset);
    while !child.is_invalid() {
        if host.data(child).kind.get() == NodeKind::LegendBox && !node_is_out_of_flow(host, child) {
            return child;
        }
        child = host.next_sibling(child);
    }
    NodeSlotId::INVALID
}

fn wrap_fieldset_contents_if_needed(host: &TreeBuilderHost<'_>, layout_node: LayoutNode) {
    assert!(!layout_node.is_invalid());

    // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
    // The anonymous fieldset content box is expected to appear after the rendered legend and is expected to contain
    // the content (including the '::before' and '::after' pseudo-elements) of the fieldset element except for the
    // rendered legend, if there is one.
    if host.data(layout_node).kind.get() == NodeKind::FieldSetBox {
        let legend = rendered_legend(host, layout_node);
        if legend.is_invalid() {
            return;
        }

        let mut children = Vec::new();
        let mut child = host.first_child(layout_node);
        while !child.is_invalid() {
            if child != legend {
                children.push(child);
            }
            child = host.next_sibling(child);
        }

        // SAFETY: `layout_node` is a live fieldset box.
        let overrides = unsafe {
            (host.callbacks.take_fieldset_overflow_for_content_wrapper)(host.callbacks.context, host.shell(layout_node))
        };
        let wrapper = host.create_anonymous_box(
            layout_node,
            FfiAnonymousStyleKind::FieldsetContentWrapper,
            overrides,
            NodeKind::BlockContainer,
        );
        let wrapper_slot = wrapper.slot();
        host.attach_child(layout_node, wrapper, NodeSlotId::INVALID);
        for child in children {
            host.move_child(child, wrapper_slot, NodeSlotId::INVALID);
        }
    }
}

fn is_table_track(display: FfiDisplay) -> bool {
    display.is_table_row() || display.is_table_column()
}

fn is_table_track_group(display: FfiDisplay) -> bool {
    // Unless explicitly mentioned otherwise, mentions of table-row-groups in this spec also encompass the specialized
    // table-header-groups and table-footer-groups.
    display.is_table_row_group()
        || display.is_table_header_group()
        || display.is_table_footer_group()
        || display.is_table_column_group()
}

fn display_for_table_fixup(host: &TreeBuilderHost<'_>, node: LayoutNode) -> FfiDisplay {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // For the purposes of these rules, out-of-flow elements are represented as inline elements of zero width and
    // height. Their containing blocks are chosen accordingly.
    //
    // AD-HOC: Table-internal boxes can be blockified before fixup. Use the pre-transformation display for ordinary
    // authored boxes so an out-of-flow table-header-group is still recognized as a proper table child during fixup.
    // Element-specific display adjustments for replaced elements and buttons take precedence over that display.
    if node_has_replaced_element_table_display_adjustment(host, node)
        || node_has_flag(host.data(node), NodeFlag::Anonymous)
        || node_has_flag(host.data(node), NodeFlag::UsesButtonLayout)
    {
        host.display(node)
    } else {
        host.display_before_box_type_transformation(node)
    }
}

fn is_proper_table_child(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    let display = display_for_table_fixup(host, node);
    is_table_track_group(display) || is_table_track(display) || display.is_table_caption()
}

fn is_table_non_root_box_with_display(display: FfiDisplay) -> bool {
    display.is_internal_table() || display.is_table_caption()
}

fn is_table_non_root_box(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    is_table_non_root_box_with_display(host.display(node))
}

fn is_tabular_container(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    // https://drafts.csswg.org/css-tables-3/#tabular-container
    let display = host.display(node);
    display.is_table_inside()
        || display.is_table_row()
        || display.is_table_row_group()
        || display.is_table_header_group()
        || display.is_table_footer_group()
}

fn text_is_ascii_whitespace(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    // SAFETY: Tree building owns the arena; no borrowed node data crosses the refresh.
    unsafe { super::rendered_text::ensure_text_content(host.arena, node) };
    host.arena()
        .text_content(node)
        .expect("text was just refreshed")
        .text
        .iter()
        .all(|unit| matches!(unit, 0x09..=0x0d | 0x20))
}

fn is_ignorable_whitespace(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    if node_kind_is_text(host.data(node).kind.get()) && text_is_ascii_whitespace(host, node) {
        return true;
    }

    // Text refresh can publish a new rendered snapshot. Borrow node data
    // again after it returns instead of retaining a reader across publication.
    let data = host.data(node);
    if node_has_flag(data, NodeFlag::Anonymous)
        && node_kind_is_block_container(data.kind.get())
        && node_has_flag(data, NodeFlag::ChildrenAreInline)
    {
        let mut contains_only_whitespace = true;
        host.for_each_in_inclusive_subtree(node, |descendant| {
            let descendant_data = host.data(descendant);
            if node_kind_is_text(descendant_data.kind.get()) {
                if !text_is_ascii_whitespace(host, descendant) {
                    contains_only_whitespace = false;
                    return TraversalDecision::Break;
                }
            } else if node_is_out_of_flow(host, descendant) || !node_has_flag(descendant_data, NodeFlag::Anonymous) {
                contains_only_whitespace = false;
                return TraversalDecision::Break;
            }
            TraversalDecision::Continue
        });
        return contains_only_whitespace;
    }

    false
}

fn is_first_or_last_child_with_table_non_root_sibling_if_any(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    let previous_sibling = host.previous_sibling(node);
    let next_sibling = host.next_sibling(node);
    if !previous_sibling.is_invalid() && !next_sibling.is_invalid() {
        return false;
    }
    if !previous_sibling.is_invalid() && !is_table_non_root_box(host, previous_sibling) {
        return false;
    }
    if !next_sibling.is_invalid() && !is_table_non_root_box(host, next_sibling) {
        return false;
    }
    true
}

fn for_each_sequence_of_consecutive_children_matching(
    host: &TreeBuilderHost<'_>,
    parent: LayoutNode,
    matcher: impl Fn(LayoutNode) -> bool,
    mut callback: impl FnMut(&[LayoutNode], LayoutNode),
) {
    let mut sequence = Vec::new();
    let mut child = host.first_child(parent);
    while !child.is_invalid() {
        if matcher(child) || (!sequence.is_empty() && is_ignorable_whitespace(host, child)) {
            sequence.push(child);
        } else if !sequence.is_empty() {
            if !sequence.iter().all(|&node| is_ignorable_whitespace(host, node)) {
                callback(&sequence, child);
            }
            sequence.clear();
        }
        child = host.next_sibling(child);
    }
    if !sequence.is_empty() && !sequence.iter().all(|&node| is_ignorable_whitespace(host, node)) {
        callback(&sequence, NodeSlotId::INVALID);
    }
}

fn remove_irrelevant_boxes(host: &TreeBuilderHost<'_>, root: LayoutNode) {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 1. Remove irrelevant boxes:
    // The following boxes are discarded as if they were display:none:
    let mut to_remove = Vec::new();
    host.for_each_in_inclusive_subtree(root, |node| {
        let data = host.data(node);

        // 1. Children of a table-column.
        if node_kind_is_box(data.kind.get()) && host.display(node).is_table_column() {
            host.set_children_are_inline(node, false);
            let mut child = host.first_child(node);
            while !child.is_invalid() {
                to_remove.push(child);
                child = host.next_sibling(child);
            }
        }

        // 2. Children of a table-column-group which are not a table-column.
        if node_kind_is_box(data.kind.get()) && host.display(node).is_table_column_group() {
            host.set_children_are_inline(node, false);
            let mut child = host.first_child(node);
            while !child.is_invalid() {
                if !host.display(child).is_table_column() {
                    to_remove.push(child);
                }
                child = host.next_sibling(child);
            }
        }

        // FIXME: 3. Anonymous inline boxes which contain only white space and are between two immediate siblings each
        //           of which is a table-non-root box.

        // 4. Anonymous inline boxes which meet all of the following criteria:
        //    - they contain only white space
        //    - they are the first and/or last child of a tabular container
        //    - whose immediate sibling, if any, is a table-non-root box
        let parent = host.parent(node);
        if node_kind_is_box(data.kind.get())
            && !parent.is_invalid()
            && is_tabular_container(host, parent)
            && !node_has_flag(host.data(parent), NodeFlag::Anonymous)
            && is_first_or_last_child_with_table_non_root_sibling_if_any(host, node)
            && is_ignorable_whitespace(host, node)
        {
            to_remove.push(node);
            return TraversalDecision::SkipChildrenAndContinue;
        }
        TraversalDecision::Continue
    });
    host.remove_nodes(&to_remove);
}

fn generate_missing_child_wrappers(host: &TreeBuilderHost<'_>, root: LayoutNode) {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 2. Generate missing child wrappers:
    host.for_each_in_inclusive_subtree(root, |parent| {
        let data = host.data(parent);
        if !node_kind_is_box(data.kind.get()) {
            return TraversalDecision::Continue;
        }
        // AD-HOC: SVG layout derives box types from the element, so display values must not introduce anonymous boxes
        //         inside SVG content.
        if node_kind_is_svg_box(data.kind.get()) || data.kind.get() == NodeKind::SVGSVGBox {
            return TraversalDecision::Continue;
        }

        let display = host.display(parent);
        if display.is_table_inside() {
            // 1. An anonymous table-row box must be generated around each sequence of consecutive children of a
            //    table-root box which are not proper table child boxes.
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| !node_has_flag(host.data(child), NodeFlag::HasStyle) || !is_proper_table_child(host, child),
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableRow);
                },
            );
        } else if display.is_table_row_group() || display.is_table_header_group() || display.is_table_footer_group() {
            // 2. An anonymous table-row box must be generated around each sequence of consecutive children of a
            //    table-row-group box which are not table-row boxes.
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| !node_has_flag(host.data(child), NodeFlag::HasStyle) || !host.display(child).is_table_row(),
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableRow);
                },
            );
        } else if display.is_table_row() {
            // 3. An anonymous table-cell box must be generated around each sequence of consecutive children of a
            //    table-row box which are not table-cell boxes.
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| !node_has_flag(host.data(child), NodeFlag::HasStyle) || !host.display(child).is_table_cell(),
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableCell);
                },
            );
        }
        TraversalDecision::Continue
    });
}

fn generate_missing_parents(host: &TreeBuilderHost<'_>, root: LayoutNode) -> Vec<LayoutNode> {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 3. Generate missing parents:
    let mut table_roots_to_wrap = Vec::new();
    host.for_each_in_inclusive_subtree(root, |parent| {
        let (has_style, is_box, kind) = {
            let data = host.data(parent);
            (
                node_has_flag(data, NodeFlag::HasStyle),
                node_kind_is_box(data.kind.get()),
                data.kind.get(),
            )
        };
        let current_display = host.display(parent);
        let is_inline_outside = node_is_inline_outside(host, parent);
        if !has_style {
            return TraversalDecision::Continue;
        }
        let node_is_svg_content = node_kind_is_svg_box(kind) || kind == NodeKind::SVGSVGBox;

        // 1. An anonymous table-row box must be generated around each sequence of consecutive table-cell boxes whose
        //    parent is not a table-row.
        if !node_is_svg_content && !current_display.is_table_row() {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| node_has_flag(host.data(child), NodeFlag::HasStyle) && host.display(child).is_table_cell(),
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableRow);
                },
            );
        }

        // 2. An anonymous table or inline-table box must be generated around each sequence of consecutive proper table
        //    child boxes which are misparented.
        // If the box’s parent is an inline, run-in, or ruby box (or any box that would perform inlinification of its
        // children), then an inline-table box must be generated; otherwise it must be a table box.
        // FIXME: run-in and ruby boxes
        let anonymous_table_kind = if is_inline_outside {
            FfiAnonymousTableBoxKind::InlineTable
        } else {
            FfiAnonymousTableBoxKind::Table
        };

        let is_table_row_group = current_display.is_table_row_group()
            || current_display.is_table_header_group()
            || current_display.is_table_footer_group();
        // A table-row is misparented if its parent is neither a table-row-group nor a table-root box.
        if !node_is_svg_content && !is_table_row_group && !current_display.is_table_inside() {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| node_has_flag(host.data(child), NodeFlag::HasStyle) && host.display(child).is_table_row(),
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, anonymous_table_kind);
                },
            );
        }

        // A table-column box is misparented if its parent is neither a table-column-group box nor a table-root box.
        if !node_is_svg_content && !current_display.is_table_column_group() && !current_display.is_table_inside() {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| node_has_flag(host.data(child), NodeFlag::HasStyle) && host.display(child).is_table_column(),
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, anonymous_table_kind);
                },
            );
        }

        // A table-row-group, table-column-group, or table-caption box is misparented if its parent is not a table-root
        // box.
        if !node_is_svg_content && !current_display.is_table_inside() {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| {
                    if !node_has_flag(host.data(child), NodeFlag::HasStyle) {
                        return false;
                    }
                    let display = display_for_table_fixup(host, child);
                    is_table_track_group(display) || display.is_table_caption()
                },
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, anonymous_table_kind);
                },
            );
        }

        // 3. An anonymous table-wrapper box must be generated around each table-root.
        if is_box && current_display.is_table_inside() {
            let wrap_parent = host.parent(parent);
            let wrap_parent_is_svg_content = !wrap_parent.is_invalid() && {
                let wrap_parent_kind = host.data(wrap_parent).kind.get();
                node_kind_is_svg_box(wrap_parent_kind) || wrap_parent_kind == NodeKind::SVGSVGBox
            };
            if !wrap_parent_is_svg_content {
                table_roots_to_wrap.push(parent);
            }
        }

        TraversalDecision::Continue
    });

    for &table_root in &table_roots_to_wrap {
        let nearest_sibling = host.next_sibling(table_root);
        let parent = host.parent(table_root);
        assert!(!parent.is_invalid());
        if host.data(parent).kind.get() != NodeKind::TableWrapper {
            let wrapper = host.create_anonymous_box(
                table_root,
                FfiAnonymousStyleKind::TableWrapper,
                FfiAnonymousStyleOverrides::default(),
                NodeKind::TableWrapper,
            );
            host.arena().reset_table_box_style_used_by_wrapper(table_root);
            let wrapper_slot = wrapper.slot();
            host.move_child(table_root, wrapper_slot, NodeSlotId::INVALID);
            host.attach_child(parent, wrapper, nearest_sibling);
        }
    }
    table_roots_to_wrap
}

fn fixup_row(
    host: &TreeBuilderHost<'_>,
    row: LayoutNode,
    table_grid: &table_formatting_context::TableGrid,
    row_index: usize,
) {
    for column_index in 0..table_grid.column_count {
        if table_grid.occupancy.contains(&(column_index, row_index)) {
            continue;
        }
        let cell = host.create_anonymous_box(
            row,
            FfiAnonymousStyleKind::MissingTableCell,
            FfiAnonymousStyleOverrides::default(),
            NodeKind::BlockContainer,
        );
        host.arena()
            .set_node_flag(cell.slot(), NodeFlag::IsMissingTableCell, true);
        host.attach_child(row, cell, NodeSlotId::INVALID);
    }
}

fn remove_missing_table_cells(host: &TreeBuilderHost<'_>, table_root: LayoutNode) {
    let mut cells = Vec::new();
    host.for_each_in_inclusive_subtree(table_root, |node| {
        let data = host.data(node);
        if node != table_root
            && node_kind_is_box(data.kind.get())
            && display_for_table_fixup(host, node).is_table_inside()
        {
            return TraversalDecision::SkipChildrenAndContinue;
        }
        if node_has_flag(data, NodeFlag::IsMissingTableCell) {
            cells.push(node);
            return TraversalDecision::SkipChildrenAndContinue;
        }
        TraversalDecision::Continue
    });
    host.remove_nodes(&cells);
}

fn missing_cells_fixup(host: &TreeBuilderHost<'_>, table_roots: &[LayoutNode]) {
    // https://drafts.csswg.org/css-tables-3/#missing-cells-fixup
    // Once the amount of columns in a table is known, any table-row box must be modified such that it owns enough
    // cells to fill all the columns of the table, when taking spans into account. New table-cell anonymous boxes must
    // be appended to its rows content until this condition is met.
    for &table_root in table_roots {
        remove_missing_table_cells(host, table_root);
        let grid = table_formatting_context::calculate_table_grid(host, table_root);
        for (row_index, row) in grid.rows.iter().enumerate() {
            fixup_row(host, row.box_, &grid, row_index);
        }
    }
}

fn table_child_is_properly_parented(parent_display: FfiDisplay, child_display: FfiDisplay) -> bool {
    if parent_display.is_table_inside() {
        return is_table_track_group(child_display)
            || is_table_track(child_display)
            || child_display.is_table_caption();
    }
    if parent_display.is_table_row_group()
        || parent_display.is_table_header_group()
        || parent_display.is_table_footer_group()
    {
        return child_display.is_table_row();
    }
    if parent_display.is_table_row() {
        return child_display.is_table_cell();
    }
    if parent_display.is_table_column_group() {
        return child_display.is_table_column();
    }
    false
}

fn table_fixup_scope_for_rebuilt_subtree(host: &TreeBuilderHost<'_>, root: LayoutNode) -> LayoutNode {
    let parent = host.parent(root);
    if parent.is_invalid() {
        return root;
    }

    let parent_display = display_for_table_fixup(host, parent);
    let parent_requires_table_children = is_tabular_container(host, parent) || parent_display.is_table_column_group();
    let root_data = host.data(root);
    if node_kind_is_text(root_data.kind.get()) || !node_has_flag(root_data, NodeFlag::HasStyle) {
        let mut ancestor = parent;
        while !ancestor.is_invalid() {
            let ancestor_data = host.data(ancestor);
            let ancestor_display = display_for_table_fixup(host, ancestor);
            if is_tabular_container(host, ancestor) || ancestor_display.is_table_column_group() {
                return ancestor;
            }
            if !node_has_flag(ancestor_data, NodeFlag::Anonymous) {
                break;
            }
            ancestor = host.parent(ancestor);
        }
        return root;
    }

    let root_display = display_for_table_fixup(host, root);
    if table_child_is_properly_parented(parent_display, root_display) {
        return root;
    }
    if parent_requires_table_children || is_table_non_root_box_with_display(root_display) {
        return parent;
    }
    root
}

fn append_unique_node(nodes: &mut Vec<LayoutNode>, node: LayoutNode) {
    if !nodes.contains(&node) {
        nodes.push(node);
    }
}

fn nearest_table_root(host: &TreeBuilderHost<'_>, node: LayoutNode) -> Option<LayoutNode> {
    let mut current = node;
    while !current.is_invalid() {
        let data = host.data(current);
        if node_has_flag(data, NodeFlag::HasStyle) && display_for_table_fixup(host, current).is_table_inside() {
            return Some(current);
        }
        current = host.parent(current);
    }
    None
}

fn fixup_tables_in_rebuilt_subtrees(
    host: &TreeBuilderHost<'_>,
    rebuilt_subtree_roots: &[LayoutNode],
    reused_child_list_update_roots: &[LayoutNode],
    additional_roots: &[LayoutNode],
) {
    let mut roots = Vec::new();
    for &root in rebuilt_subtree_roots {
        if host.arena().node_data_if_live(root).is_none() || host.parent(root).is_invalid() {
            continue;
        }
        let scope = table_fixup_scope_for_rebuilt_subtree(host, root);
        append_unique_node(&mut roots, scope);
    }
    for &root in reused_child_list_update_roots {
        if host.arena().node_data_if_live(root).is_none() || host.parent(root).is_invalid() {
            continue;
        }
        let has_live_rebuilt_descendant = rebuilt_subtree_roots.iter().any(|&rebuilt_root| {
            host.arena().node_data_if_live(rebuilt_root).is_some()
                && is_inclusive_layout_ancestor_of(host, root, rebuilt_root)
        });
        if !has_live_rebuilt_descendant {
            append_unique_node(&mut roots, root);
        }
    }
    for &root in additional_roots {
        if host.arena().node_data_if_live(root).is_none() || host.parent(root).is_invalid() {
            continue;
        }
        append_unique_node(&mut roots, root);
    }

    let candidate_roots = roots.clone();
    roots.retain(|&candidate| {
        !candidate_roots
            .iter()
            .any(|&other| other != candidate && is_inclusive_layout_ancestor_of(host, other, candidate))
    });

    let mut table_roots = Vec::new();
    for &root in &roots {
        if let Some(table_root) = nearest_table_root(host, root) {
            append_unique_node(&mut table_roots, table_root);
        }
    }

    for &root in &roots {
        remove_irrelevant_boxes(host, root);
    }
    for &root in &roots {
        generate_missing_child_wrappers(host, root);
    }
    for &root in &roots {
        for table_root in generate_missing_parents(host, root) {
            append_unique_node(&mut table_roots, table_root);
        }
    }
    missing_cells_fixup(host, &table_roots);
}

fn fixup_tables(host: &TreeBuilderHost<'_>, root: LayoutNode) {
    assert!(!root.is_invalid());
    remove_irrelevant_boxes(host, root);
    generate_missing_child_wrappers(host, root);
    let table_roots = generate_missing_parents(host, root);
    missing_cells_fixup(host, &table_roots);
}

#[cfg(test)]
mod tests {
    use crate::layout::node_data::NodeSlotId;
    use crate::layout::tree_builder::{
        FfiCodePointCategoryFacts, FfiComputedContentType, FfiElementLayoutFacts, FfiElementLayoutKind,
        FfiPrincipalBoxPlacement, FfiPrincipalNodeEntryFacts, FfiPseudoElement, FfiPseudoElementDecision,
        FfiPseudoElementFacts, FfiReplacedElementDisplayAdjustment, PrincipalBoxGenerationDecision,
        PrincipalBoxPlacementFacts, SvgEntryDecision, TopLayerEntryDecision, TreeBuilderContext,
        adjusted_table_display_for_replaced_element, display_contents_text_needs_style_wrapper, element_layout_kind,
        find_first_letter_in_text, principal_box_generation_decision, principal_box_placement_decision,
        principal_node_entry_decision, pseudo_element_decision,
    };
    use std::ffi::c_void;

    fn code_point_facts(code_point: u32) -> FfiCodePointCategoryFacts {
        FfiCodePointCategoryFacts {
            is_space_separator: code_point == b' ' as u32,
            is_punctuation: matches!(code_point, 0x21 | 0x22 | 0x27..=0x2f | 0x3a | 0x3b | 0x3f | 0x40),
            is_letter: matches!(code_point, 0x41..=0x5a | 0x61..=0x7a),
            is_number: matches!(code_point, 0x30..=0x39),
            is_symbol: matches!(code_point, 0x24 | 0x2b | 0x3c..=0x3e | 0x5e | 0x60 | 0x7c | 0x7e),
            is_open_punctuation: matches!(code_point, 0x28 | 0x5b | 0x7b),
            is_dash_punctuation: code_point == 0x2d,
        }
    }

    fn first_letter_target(
        text: &str,
        preserves_segment_breaks: bool,
    ) -> crate::layout::tree_builder::FfiFirstLetterTarget {
        let text = text.encode_utf16().collect::<Vec<_>>();
        find_first_letter_in_text(&text, preserves_segment_breaks, |index| index + 1, code_point_facts)
    }

    #[test]
    fn replaced_table_display_adjustments() {
        assert_eq!(
            adjusted_table_display_for_replaced_element(true, true, false, false),
            FfiReplacedElementDisplayAdjustment::Block
        );
        assert_eq!(
            adjusted_table_display_for_replaced_element(true, false, false, false),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            adjusted_table_display_for_replaced_element(false, false, true, false),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            adjusted_table_display_for_replaced_element(false, false, false, true),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            adjusted_table_display_for_replaced_element(false, false, false, false),
            FfiReplacedElementDisplayAdjustment::None
        );
    }

    #[test]
    fn first_letter_text_pattern() {
        let target = first_letter_target("  Hello", false);
        assert!(target.found);
        assert_eq!((target.letter_start, target.letter_end), (2, 3));

        let target = first_letter_target("\") A", false);
        assert!(target.found);
        assert_eq!((target.letter_start, target.letter_end), (0, 4));

        let target = first_letter_target("H!ello", false);
        assert!(target.found);
        assert_eq!((target.letter_start, target.letter_end), (0, 2));

        let target = first_letter_target("H-ello", false);
        assert!(target.found);
        assert_eq!((target.letter_start, target.letter_end), (0, 1));

        assert!(!first_letter_target("\nHello", true).found);
    }

    #[test]
    fn first_letter_source_matching_decodes_surrogates_and_respects_grapheme_boundaries() {
        let text: Vec<u16> = "  😀abc".encode_utf16().collect();
        let target = find_first_letter_in_text(
            &text,
            false,
            |index| if index == 2 { 4 } else { index + 1 },
            |code_point| {
                let mut facts = code_point_facts(code_point);
                facts.is_symbol |= code_point == 0x1f600;
                facts
            },
        );
        assert!(target.found);
        assert_eq!(
            (target.letter_start, target.letter_end, target.source_length),
            (2, 4, 7)
        );

        let text: Vec<u16> = "I\u{0307}abc".encode_utf16().collect();
        let target = find_first_letter_in_text(
            &text,
            false,
            |index| if index == 0 { 2 } else { index + 1 },
            code_point_facts,
        );
        assert!(target.found);
        assert_eq!((target.letter_start, target.letter_end), (0, 2));
    }

    #[test]
    fn tree_builder_state_tracks_ancestors_and_quotes() {
        let mut state = crate::layout::tree_builder::TreeBuilderState::default();
        let parent = NodeSlotId { index: 42 };
        state.ancestor_stack.push(parent);
        assert_eq!(state.ancestor_stack.len(), 1);
        assert_eq!(state.current_parent(), parent);
        assert_eq!(state.ancestor_stack[0], parent);

        state.quote_nesting_level = 3;
        assert_eq!(state.quote_nesting_level, 3);

        assert!(state.ancestor_stack.pop().is_some());
        assert_eq!(state.ancestor_stack.len(), 0);
    }

    #[test]
    fn pseudo_element_box_generation_decisions() {
        let decide = |pseudo_element,
                      content_type,
                      display_is_none,
                      display_is_contents,
                      display_is_list_item,
                      has_content_replacement,
                      originating_layout_node_is_list_item,
                      normal_marker_has_content| {
            pseudo_element_decision(FfiPseudoElementFacts {
                has_style: true,
                pseudo_element,
                content_type,
                display_is_none,
                display_is_contents,
                display_is_list_item,
                has_content_replacement,
                originating_layout_node_is_list_item,
                normal_marker_has_content,
                marker_position_is_inside: false,
            })
        };

        assert_eq!(
            decide(
                FfiPseudoElement::Before,
                FfiComputedContentType::Normal,
                false,
                false,
                false,
                false,
                false,
                false
            ),
            FfiPseudoElementDecision::None
        );
        assert_eq!(
            decide(
                FfiPseudoElement::Marker,
                FfiComputedContentType::Normal,
                false,
                false,
                false,
                false,
                true,
                true
            ),
            FfiPseudoElementDecision::Box
        );
        assert_eq!(
            decide(
                FfiPseudoElement::Other,
                FfiComputedContentType::List,
                false,
                false,
                false,
                true,
                false,
                false
            ),
            FfiPseudoElementDecision::ContentReplacement
        );
        assert_eq!(
            decide(
                FfiPseudoElement::Other,
                FfiComputedContentType::List,
                false,
                true,
                false,
                true,
                false,
                false
            ),
            FfiPseudoElementDecision::Contents
        );
        assert_eq!(
            decide(
                FfiPseudoElement::Other,
                FfiComputedContentType::List,
                false,
                false,
                true,
                true,
                false,
                false
            ),
            FfiPseudoElementDecision::Box
        );
    }

    #[test]
    fn principal_node_entry_decisions() {
        let mut facts = FfiPrincipalNodeEntryFacts {
            must_create_subtree: false,
            needs_layout_tree_update: false,
            may_reuse_layout_node_for_child_list_insertion: false,
            document_needs_full_layout_tree_update: false,
            is_document: false,
            has_layout_node: true,
            is_element: true,
            is_text: false,
            rendered_in_top_layer: false,
            layout_node_is_attached: true,
            is_svg_container: false,
            requires_svg_container: false,
            is_svg_foreign_object: false,
        };
        let mut context = TreeBuilderContext::default();
        let decision = principal_node_entry_decision(facts, &context);
        assert!(!decision.should_create_layout_node);
        assert_eq!(decision.top_layer, TopLayerEntryDecision::Continue);
        assert_eq!(decision.svg, SvgEntryDecision::Continue);

        facts.rendered_in_top_layer = true;
        facts.layout_node_is_attached = false;
        let decision = principal_node_entry_decision(facts, &context);
        assert_eq!(decision.top_layer, TopLayerEntryDecision::SkipAndRequestZoneRebuild);

        facts.rendered_in_top_layer = false;
        facts.requires_svg_container = true;
        let decision = principal_node_entry_decision(facts, &context);
        assert_eq!(decision.svg, SvgEntryDecision::Skip);

        facts.must_create_subtree = true;
        facts.is_svg_container = true;
        context.has_svg_root = false;
        let decision = principal_node_entry_decision(facts, &context);
        assert!(decision.should_create_layout_node);
        assert_eq!(decision.svg, SvgEntryDecision::EnterSvgRoot);

        facts.is_svg_container = false;
        facts.is_svg_foreign_object = true;
        context.has_svg_root = true;
        let decision = principal_node_entry_decision(facts, &context);
        assert_eq!(decision.svg, SvgEntryDecision::EnterForeignContent);
        context.has_svg_root = false;
        let decision = principal_node_entry_decision(facts, &context);
        assert_eq!(decision.svg, SvgEntryDecision::Skip);

        facts.is_svg_foreign_object = false;
        facts.requires_svg_container = false;
        context.has_svg_root = true;
        let decision = principal_node_entry_decision(facts, &context);
        assert_eq!(decision.svg, SvgEntryDecision::Skip);
        context.has_svg_root = false;
        let decision = principal_node_entry_decision(facts, &context);
        assert_eq!(decision.svg, SvgEntryDecision::Continue);
    }

    #[test]
    fn specialized_element_layout_kinds() {
        let mut facts = FfiElementLayoutFacts {
            has_content_replacement: false,
            is_svg_mask_element: false,
            is_svg_clip_path_element: false,
            is_svg_pattern_element: false,
        };
        assert_eq!(element_layout_kind(facts, false, false), FfiElementLayoutKind::Normal);

        facts.has_content_replacement = true;
        assert_eq!(
            element_layout_kind(facts, false, false),
            FfiElementLayoutKind::ContentReplacement
        );
        facts.has_content_replacement = false;
        facts.is_svg_mask_element = true;
        assert_eq!(element_layout_kind(facts, true, false), FfiElementLayoutKind::SvgMask);

        facts.is_svg_mask_element = false;
        facts.is_svg_pattern_element = true;
        assert_eq!(
            element_layout_kind(facts, false, true),
            FfiElementLayoutKind::SvgPattern
        );
    }

    #[test]
    fn principal_box_generation_and_placement_decisions() {
        assert_eq!(
            principal_box_generation_decision(true, true, false),
            PrincipalBoxGenerationDecision::Suppress
        );
        assert_eq!(
            principal_box_generation_decision(true, false, true),
            PrincipalBoxGenerationDecision::DisplayContents
        );
        assert_eq!(
            principal_box_generation_decision(false, false, false),
            PrincipalBoxGenerationDecision::PrincipalBox
        );

        let mut facts = PrincipalBoxPlacementFacts {
            must_create_subtree: false,
            should_create_layout_node: true,
            has_old_layout_node: true,
            old_layout_node_is_attached: true,
            old_and_new_layout_nodes_are_same: false,
            has_current_rebuild_root: false,
            is_in_dom_order_insertion: false,
            is_document: false,
            is_element: true,
            rendered_in_top_layer: true,
        };
        let decision = principal_box_placement_decision(facts, false, true);
        assert_eq!(decision.placement, FfiPrincipalBoxPlacement::ReplaceExisting);
        assert!(decision.start_rebuild_root);
        assert!(decision.create_backdrop);
        assert!(decision.clear_layout_top_layer_for_descendants);

        facts.has_old_layout_node = false;
        facts.old_layout_node_is_attached = false;
        facts.is_in_dom_order_insertion = true;
        let decision = principal_box_placement_decision(facts, false, false);
        assert_eq!(decision.placement, FfiPrincipalBoxPlacement::NormalInsertion);
        assert!(decision.start_rebuild_root);
        assert!(!decision.mark_update_escaped_rebuild_roots);

        facts.is_in_dom_order_insertion = false;
        let decision = principal_box_placement_decision(facts, true, true);
        assert_eq!(decision.placement, FfiPrincipalBoxPlacement::AppendSvg);
        assert!(decision.mark_update_escaped_rebuild_roots);
    }

    #[test]
    fn display_contents_text_style_wrapper_decisions() {
        assert!(!display_contents_text_needs_style_wrapper(false, true, false, false));
        assert!(display_contents_text_needs_style_wrapper(true, true, false, true));
        assert!(!display_contents_text_needs_style_wrapper(true, true, true, true));
        assert!(display_contents_text_needs_style_wrapper(true, true, true, false));
    }
}
