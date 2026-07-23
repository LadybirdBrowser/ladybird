/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
use std::ffi::c_void;

struct TreeBuilderState {
    ancestor_stack: Vec<LayoutNode>,
    quote_nesting_level: u32,
}

/// Creates the Rust-owned state for one layout tree builder.
#[unsafe(no_mangle)]
pub extern "C" fn rust_tree_builder_state_create() -> *mut c_void {
    abort_on_panic(|| {
        Box::into_raw(Box::new(TreeBuilderState {
            ancestor_stack: Vec::new(),
            quote_nesting_level: 0,
        }))
        .cast()
    })
}

/// Destroys state returned by `rust_tree_builder_state_create`.
///
/// # Safety
///
/// `state` must have been returned by `rust_tree_builder_state_create` and must not have been destroyed already.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_tree_builder_state_destroy(state: *mut c_void) {
    abort_on_panic(|| {
        assert!(!state.is_null());
        // SAFETY: Guaranteed by the entry point's contract and checked for null above.
        drop(unsafe { Box::from_raw(state.cast::<TreeBuilderState>()) });
    });
}

fn tree_builder_state(state: *mut c_void) -> &'static TreeBuilderState {
    assert!(!state.is_null());
    // SAFETY: Every caller passes live state created by `rust_tree_builder_state_create`.
    unsafe { &*state.cast::<TreeBuilderState>() }
}

fn tree_builder_state_mut(state: *mut c_void) -> &'static mut TreeBuilderState {
    assert!(!state.is_null());
    // SAFETY: FFI calls are serialized by the owning C++ TreeBuilder.
    unsafe { &mut *state.cast::<TreeBuilderState>() }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_tree_builder_push_parent(state: *mut c_void, parent: *mut c_void) {
    abort_on_panic(|| {
        assert!(!parent.is_null());
        tree_builder_state_mut(state).ancestor_stack.push(parent);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_tree_builder_pop_parent(state: *mut c_void) {
    abort_on_panic(|| {
        assert!(tree_builder_state_mut(state).ancestor_stack.pop().is_some());
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_tree_builder_ancestor_count(state: *mut c_void) -> usize {
    abort_on_panic(|| tree_builder_state(state).ancestor_stack.len())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_tree_builder_ancestor_at(state: *mut c_void, index: usize) -> *mut c_void {
    abort_on_panic(|| tree_builder_state(state).ancestor_stack[index])
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_tree_builder_current_parent(state: *mut c_void) -> *mut c_void {
    abort_on_panic(|| {
        *tree_builder_state(state)
            .ancestor_stack
            .last()
            .expect("layout tree builder must have an insertion ancestor")
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_tree_builder_quote_nesting_level(state: *mut c_void) -> u32 {
    abort_on_panic(|| tree_builder_state(state).quote_nesting_level)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_tree_builder_set_quote_nesting_level(state: *mut c_void, quote_nesting_level: u32) {
    abort_on_panic(|| {
        tree_builder_state_mut(state).quote_nesting_level = quote_nesting_level;
    });
}

#[repr(C)]
pub struct FfiDomTreeBuilderCallbacks {
    pub builder: *mut c_void,
    pub first_child: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub next_sibling: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub update_layout_tree: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, bool),
    pub clear_update_flags: unsafe extern "C" fn(*mut c_void),
    pub needs_layout_tree_update: unsafe extern "C" fn(*mut c_void) -> bool,
    pub assigned_node_count: unsafe extern "C" fn(*mut c_void) -> usize,
    pub assigned_node_at: unsafe extern "C" fn(*mut c_void, usize) -> *mut c_void,
    pub is_svg_element: unsafe extern "C" fn(*mut c_void) -> bool,
    pub clear_stale_layout_and_paint_node: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub display_contents_facts: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> FfiDisplayContentsFacts,
    pub set_context_layout_top_layer: unsafe extern "C" fn(*mut c_void, bool),
    pub clear_synthetic_pseudo_element_layout_nodes: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub clear_stale_inclusive_descendants: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub resolve_counters: unsafe extern "C" fn(*mut c_void),
    pub create_pseudo_element: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiPseudoElement, FfiInsertionMode),
    pub clear_stale_assigned_slottables: unsafe extern "C" fn(*mut c_void),
    pub principal_descendant_facts:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, *mut c_void) -> FfiPrincipalDescendantFacts,
    pub create_first_letter_wrapper: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub wrap_fieldset_layout: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub wrap_button_layout: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub clear_stale_descendants: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub push_layout_parent: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub pop_layout_parent: unsafe extern "C" fn(*mut c_void),
    pub ensure_replaced_children_wrapper: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub top_layer_element_count: unsafe extern "C" fn(*mut c_void) -> usize,
    pub copy_top_layer_elements: unsafe extern "C" fn(*mut c_void, *mut *mut c_void, usize),
    pub rendered_in_top_layer: unsafe extern "C" fn(*mut c_void) -> bool,
    pub flat_tree_parent: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub flat_tree_render_facts: unsafe extern "C" fn(*mut c_void) -> FfiFlatTreeRenderFacts,
    pub clear_stale_top_layer_subtree: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub quote_nesting_level: unsafe extern "C" fn(*mut c_void) -> u32,
    pub set_quote_nesting_level: unsafe extern "C" fn(*mut c_void, u32),
    pub clear_dom_update_flags: unsafe extern "C" fn(*mut c_void),
    pub svg_pattern_content_element: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub register_svg_resource_reference: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub ancestor_count: unsafe extern "C" fn(*mut c_void) -> usize,
    pub ancestor_at: unsafe extern "C" fn(*mut c_void, usize) -> *mut c_void,
    pub layout_node_dom_node: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub set_context_layout_svg_mask_or_clip_path: unsafe extern "C" fn(*mut c_void, bool),
    pub set_context_layout_svg_pattern: unsafe extern "C" fn(*mut c_void, bool),
    pub principal_node_entry_facts:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, bool) -> FfiPrincipalNodeEntryFacts,
    pub request_top_layer_zone_rebuild: unsafe extern "C" fn(*mut c_void),
    pub push_style_ancestor: unsafe extern "C" fn(*mut c_void),
    pub pop_style_ancestor: unsafe extern "C" fn(*mut c_void),
    pub initialize_principal_frame: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub prepare_principal_element:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, bool) -> FfiPrincipalDisplayFacts,
    pub principal_element_layout_facts:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> FfiElementLayoutFacts,
    pub create_principal_element_layout:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, FfiElementLayoutKind),
    pub create_principal_document_layout: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub create_principal_text_layout: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub reuse_principal_layout: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub principal_layout_node: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub attach_principal_style_resources: unsafe extern "C" fn(*mut c_void),
    pub principal_layout_facts: unsafe extern "C" fn(*mut c_void) -> FfiPrincipalLayoutFacts,
    pub apply_replaced_display_adjustment: unsafe extern "C" fn(*mut c_void, FfiReplacedElementDisplayAdjustment),
    pub principal_placement_facts: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        *mut c_void,
        *mut c_void,
        bool,
        bool,
    ) -> FfiPrincipalBoxPlacementFacts,
    pub start_principal_rebuild_root: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub restore_principal_rebuild_root: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub mark_update_escaped_rebuild_roots: unsafe extern "C" fn(*mut c_void),
    pub create_principal_backdrop: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, bool) -> *mut c_void,
    pub insert_principal_backdrop_before_old: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub place_principal_layout: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiPrincipalBoxPlacement),
    pub clear_stale_inclusive_subtree: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub set_context_has_svg_root: unsafe extern "C" fn(*mut c_void, bool),
    pub reset_style_ancestor_filter: unsafe extern "C" fn(*mut c_void),
    pub document_layout_node: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub fixup_tables: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub layout_root: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiDisplayContentsFacts {
    pub rendered_in_top_layer: bool,
    pub context_layout_top_layer: bool,
    pub content_visibility_hidden: bool,
    pub should_layout_dom_children: bool,
    pub child_needs_layout_tree_update: bool,
    pub dom_children_parent: *mut c_void,
    pub shadow_root: *mut c_void,
    pub slot_element: *mut c_void,
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
    pub context_layout_top_layer: bool,
    pub content_visibility_hidden: bool,
    pub should_layout_dom_children: bool,
    pub child_needs_layout_tree_update: bool,
    pub layout_node_can_have_children: bool,
    pub layout_node_is_replaced_box_with_children: bool,
    pub layout_node_is_list_item_box: bool,
    pub layout_node_is_block_container: bool,
    pub has_first_letter_style: bool,
    pub is_svg_switch_element: bool,
    pub is_document: bool,
    pub has_style_containment: bool,
    pub dom_children_parent: *mut c_void,
    pub shadow_root: *mut c_void,
    pub slot_element: *mut c_void,
    pub svg_graphics_element: *mut c_void,
    pub svg_mask: *mut c_void,
    pub svg_clip_path: *mut c_void,
    pub svg_fill_pattern: *mut c_void,
    pub svg_stroke_pattern: *mut c_void,
    pub context_layout_svg_mask_or_clip_path: bool,
    pub context_layout_svg_pattern: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPrincipalNodeEntryFacts {
    pub must_create_subtree: bool,
    pub needs_layout_tree_update: bool,
    pub document_needs_full_layout_tree_update: bool,
    pub is_document: bool,
    pub has_layout_node: bool,
    pub is_element: bool,
    pub is_text: bool,
    pub rendered_in_top_layer: bool,
    pub context_layout_top_layer: bool,
    pub layout_node_is_attached: bool,
    pub is_svg_container: bool,
    pub requires_svg_container: bool,
    pub context_has_svg_root: bool,
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
pub struct FfiPrincipalLayoutFacts {
    pub is_replaced_element: bool,
    pub display: FfiPrincipalDisplayFacts,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiElementLayoutFacts {
    pub has_content_replacement: bool,
    pub context_layout_svg_mask_or_clip_path: bool,
    pub context_layout_svg_pattern: bool,
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

fn element_layout_kind(facts: FfiElementLayoutFacts) -> FfiElementLayoutKind {
    if facts.has_content_replacement {
        FfiElementLayoutKind::ContentReplacement
    } else if facts.context_layout_svg_mask_or_clip_path {
        if facts.is_svg_mask_element {
            FfiElementLayoutKind::SvgMask
        } else {
            assert!(facts.is_svg_clip_path_element);
            FfiElementLayoutKind::SvgClipPath
        }
    } else if facts.context_layout_svg_pattern {
        assert!(facts.is_svg_pattern_element);
        FfiElementLayoutKind::SvgPattern
    } else {
        FfiElementLayoutKind::Normal
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TopLayerEntryDecision {
    Continue,
    Skip,
    SkipAndRequestZoneRebuild,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SvgEntryDecision {
    Continue,
    EnterSvgRoot,
    Skip,
}

#[derive(Clone, Copy)]
struct PrincipalNodeEntryDecision {
    should_create_layout_node: bool,
    top_layer: TopLayerEntryDecision,
    svg: SvgEntryDecision,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PrincipalBoxGenerationDecision {
    Suppress,
    DisplayContents,
    PrincipalBox,
}

fn principal_box_generation_decision(
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

#[unsafe(no_mangle)]
pub extern "C" fn rust_display_contents_text_needs_style_wrapper(
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
    pub layout_parent: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub layout_dom_node: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub dom_is_shadow_including_inclusive_descendant: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
}

/// Returns whether an SVG resource layout node must survive cleanup of a DOM subtree.
///
/// # Safety
///
/// The callback table, layout node, and optional cleared subtree root must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_should_preserve_svg_resource_layout_node(
    callbacks: *const FfiStaleNodeCallbacks,
    layout_node: *mut c_void,
    cleared_subtree_root: *mut c_void,
) -> bool {
    abort_on_panic(|| {
        assert!(!callbacks.is_null());
        assert!(!layout_node.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let callbacks = unsafe { &*callbacks };
        if cleared_subtree_root.is_null() {
            return true;
        }

        // SAFETY: The layout node remains live throughout the ancestor walk.
        let mut ancestor = unsafe { (callbacks.layout_parent)(layout_node) };
        while !ancestor.is_null() {
            // SAFETY: `ancestor` is a live layout node.
            let dom_node = unsafe { (callbacks.layout_dom_node)(ancestor) };
            // SAFETY: Both DOM pointers remain live throughout cleanup.
            if !dom_node.is_null()
                && unsafe { (callbacks.dom_is_shadow_including_inclusive_descendant)(dom_node, cleared_subtree_root) }
            {
                return false;
            }
            // SAFETY: `ancestor` remains live throughout the walk.
            ancestor = unsafe { (callbacks.layout_parent)(ancestor) };
        }
        true
    })
}

#[repr(C)]
pub struct FfiTopLayerDetachCallbacks {
    pub element_layout_node: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub topmost_placement_node: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub prepare_subtree_for_detach: unsafe extern "C" fn(*mut c_void),
    pub layout_parent: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub remove_layout_node: unsafe extern "C" fn(*mut c_void),
    pub clear_element_subtree: unsafe extern "C" fn(*mut c_void),
    pub slot_element: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub clear_assigned_slottables: unsafe extern "C" fn(*mut c_void),
}

/// Detaches a top-layer element's layout placement and clears every stale projected subtree.
///
/// # Safety
///
/// The callback table and element must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_detach_top_layer_element_layout_subtree(
    callbacks: *const FfiTopLayerDetachCallbacks,
    element: *mut c_void,
) {
    abort_on_panic(|| {
        assert!(!callbacks.is_null());
        assert!(!element.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let callbacks = unsafe { &*callbacks };
        // SAFETY: The element remains live throughout the call.
        let element_layout_node = unsafe { (callbacks.element_layout_node)(element) };
        if !element_layout_node.is_null() {
            // Take along any anonymous table-fixup wrapper around the box. Leaving an empty table wrapper as a
            // viewport child would violate layout invariants.
            // SAFETY: The element layout node remains live throughout detachment.
            let topmost = unsafe { (callbacks.topmost_placement_node)(element_layout_node) };
            let layout_node_to_detach = if topmost.is_null() {
                element_layout_node
            } else {
                topmost
            };
            // SAFETY: The chosen layout subtree remains live throughout detachment.
            unsafe {
                (callbacks.prepare_subtree_for_detach)(layout_node_to_detach);
                if !(callbacks.layout_parent)(layout_node_to_detach).is_null() {
                    (callbacks.remove_layout_node)(layout_node_to_detach);
                }
            }
        }

        // SAFETY: The element remains live throughout subtree cleanup.
        unsafe { (callbacks.clear_element_subtree)(element) };
        // SAFETY: The callback returns the element's adjusted HTMLSlotElement pointer, if any.
        let slot_element = unsafe { (callbacks.slot_element)(element) };
        if !slot_element.is_null() {
            // SAFETY: The slot element remains live throughout assigned-node cleanup.
            unsafe { (callbacks.clear_assigned_slottables)(slot_element) };
        }
    });
}

#[repr(C)]
pub struct FfiAssignedCleanupCallbacks {
    pub assigned_node_count: unsafe extern "C" fn(*mut c_void) -> usize,
    pub assigned_node_at: unsafe extern "C" fn(*mut c_void, usize) -> *mut c_void,
    pub clear_assigned_subtree: unsafe extern "C" fn(*mut c_void),
}

/// Clears stale layout state for every flat-tree child assigned to a slot.
///
/// # Safety
///
/// The callback table and slot element must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_clear_stale_assigned_slottables(
    callbacks: *const FfiAssignedCleanupCallbacks,
    slot_element: *mut c_void,
) {
    abort_on_panic(|| {
        assert!(!callbacks.is_null());
        assert!(!slot_element.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let callbacks = unsafe { &*callbacks };
        // SAFETY: The slot and assigned-node list remain live throughout cleanup.
        let count = unsafe { (callbacks.assigned_node_count)(slot_element) };
        for index in 0..count {
            // SAFETY: `index` is below the stable assigned-node count.
            let root = unsafe { (callbacks.assigned_node_at)(slot_element, index) };
            assert!(!root.is_null());
            // SAFETY: The assigned subtree root remains live throughout cleanup.
            unsafe { (callbacks.clear_assigned_subtree)(root) };
        }
    });
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPrincipalBoxPlacementFacts {
    pub must_create_subtree: bool,
    pub should_create_layout_node: bool,
    pub has_old_layout_node: bool,
    pub old_layout_node_is_attached: bool,
    pub old_and_new_layout_nodes_are_same: bool,
    pub has_current_rebuild_root: bool,
    pub is_document: bool,
    pub is_element: bool,
    pub rendered_in_top_layer: bool,
    pub context_layout_top_layer: bool,
    pub layout_node_is_svg_box: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiPrincipalBoxPlacement {
    None,
    DocumentRoot,
    ReplaceExisting,
    AppendSvg,
    NormalInsertion,
}

#[derive(Clone, Copy)]
struct PrincipalBoxPlacementDecision {
    placement: FfiPrincipalBoxPlacement,
    may_replace_existing_layout_node: bool,
    start_rebuild_root: bool,
    mark_update_escaped_rebuild_roots: bool,
    create_backdrop: bool,
    clear_layout_top_layer_for_descendants: bool,
}

fn principal_box_placement_decision(facts: FfiPrincipalBoxPlacementFacts) -> PrincipalBoxPlacementDecision {
    abort_on_panic(|| {
        let may_replace_existing_layout_node = !facts.must_create_subtree
            && facts.has_old_layout_node
            && facts.old_layout_node_is_attached
            && !facts.old_and_new_layout_nodes_are_same;
        let start_rebuild_root = may_replace_existing_layout_node && !facts.has_current_rebuild_root;
        let mark_update_escaped_rebuild_roots = facts.should_create_layout_node
            && !facts.has_old_layout_node
            && !facts.has_current_rebuild_root
            && !facts.is_document;

        let placement = if facts.is_document {
            FfiPrincipalBoxPlacement::DocumentRoot
        } else if !facts.should_create_layout_node {
            FfiPrincipalBoxPlacement::None
        } else if may_replace_existing_layout_node {
            FfiPrincipalBoxPlacement::ReplaceExisting
        } else if facts.layout_node_is_svg_box {
            FfiPrincipalBoxPlacement::AppendSvg
        } else {
            FfiPrincipalBoxPlacement::NormalInsertion
        };

        let is_active_top_layer_member =
            facts.is_element && facts.rendered_in_top_layer && facts.context_layout_top_layer;
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

fn principal_node_entry_decision(facts: FfiPrincipalNodeEntryFacts) -> PrincipalNodeEntryDecision {
    abort_on_panic(|| {
        let should_create_layout_node = facts.must_create_subtree
            || facts.needs_layout_tree_update
            || facts.document_needs_full_layout_tree_update
            || (facts.is_document && !facts.has_layout_node);

        let top_layer = if facts.is_element && facts.rendered_in_top_layer && !facts.context_layout_top_layer {
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
        } else if facts.requires_svg_container && !facts.context_has_svg_root {
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

    fn update_layout_tree(&self, node: *mut c_void, context: *mut c_void, must_create_subtree: bool) {
        // SAFETY: The builder, DOM node, and traversal context remain live throughout recursive construction.
        unsafe {
            (self.callbacks.update_layout_tree)(self.callbacks.builder, node, context, must_create_subtree);
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

unsafe fn dom_tree_builder_host<'a>(callbacks: *const FfiDomTreeBuilderCallbacks) -> DomTreeBuilderHost<'a> {
    assert!(!callbacks.is_null());
    // SAFETY: Each exported entry point requires the callback table to remain live for the duration of its call.
    DomTreeBuilderHost {
        callbacks: unsafe { &*callbacks },
    }
}

/// Updates every direct DOM child in tree order.
///
/// # Safety
///
/// The callback table, parent, and context must remain valid for the duration of the call.
unsafe fn update_layout_tree_for_dom_children(
    callbacks: *const FfiDomTreeBuilderCallbacks,
    parent: *mut c_void,
    context: *mut c_void,
    must_create_subtree: bool,
) {
    abort_on_panic(|| {
        assert!(!parent.is_null());
        assert!(!context.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let host = unsafe { dom_tree_builder_host(callbacks) };
        let mut node = host.first_child(parent);
        while !node.is_null() {
            host.update_layout_tree(node, context, must_create_subtree);
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
    callbacks: *const FfiDomTreeBuilderCallbacks,
    shadow_root: *mut c_void,
    context: *mut c_void,
    must_create_subtree: bool,
) {
    abort_on_panic(|| {
        assert!(!shadow_root.is_null());
        assert!(!context.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let host = unsafe { dom_tree_builder_host(callbacks) };
        let mut node = host.first_child(shadow_root);
        while !node.is_null() {
            host.update_layout_tree(node, context, must_create_subtree);
            node = host.next_sibling(node);
        }
        // SAFETY: `shadow_root` remains live throughout the call.
        unsafe { (host.callbacks.clear_update_flags)(shadow_root) };
    });
}

/// Updates a slot's assigned nodes in flat-tree order.
///
/// # Safety
///
/// The callback table, slot element, and context must remain valid for the duration of the call.
unsafe fn update_layout_tree_for_assigned_slottables(
    callbacks: *const FfiDomTreeBuilderCallbacks,
    slot_element: *mut c_void,
    context: *mut c_void,
    must_create_subtree: bool,
) {
    abort_on_panic(|| {
        assert!(!slot_element.is_null());
        assert!(!context.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let host = unsafe { dom_tree_builder_host(callbacks) };
        // SAFETY: `slot_element` remains live throughout the call.
        let slot_needs_layout_tree_update = unsafe { (host.callbacks.needs_layout_tree_update)(slot_element) };
        let must_create_subtree = must_create_subtree || slot_needs_layout_tree_update;
        // SAFETY: `slot_element` remains live throughout the call.
        let assigned_node_count = unsafe { (host.callbacks.assigned_node_count)(slot_element) };
        for index in 0..assigned_node_count {
            // SAFETY: `index` is below the count reported for this unchanged assigned-node list.
            let node = unsafe { (host.callbacks.assigned_node_at)(slot_element, index) };
            assert!(!node.is_null());
            host.update_layout_tree(node, context, must_create_subtree);
        }
    });
}

/// Applies SVG `<switch>` child selection and updates its rendered child.
///
/// # Safety
///
/// The callback table, switch element, and context must remain valid for the duration of the call.
unsafe fn update_layout_tree_for_svg_switch_children(
    callbacks: *const FfiDomTreeBuilderCallbacks,
    switch_element: *mut c_void,
    context: *mut c_void,
    must_create_subtree: bool,
) {
    abort_on_panic(|| {
        assert!(!switch_element.is_null());
        assert!(!context.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let host = unsafe { dom_tree_builder_host(callbacks) };

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
                    (host.callbacks.clear_stale_layout_and_paint_node)(host.callbacks.builder, child);
                }
            }
            child = host.next_sibling(child);
        }

        if !rendered_child.is_null() {
            host.update_layout_tree(rendered_child, context, must_create_subtree);
        }
    });
}

/// Updates an element that generates no principal box because it has `display: contents`.
///
/// # Safety
///
/// The callback table, element, and context must remain valid for the duration of the call.
unsafe fn update_layout_tree_for_display_contents(
    callbacks: *const FfiDomTreeBuilderCallbacks,
    element: *mut c_void,
    context: *mut c_void,
    must_create_subtree: bool,
    should_create_layout_node: bool,
) {
    abort_on_panic(|| {
        assert!(!element.is_null());
        assert!(!context.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let host = unsafe { dom_tree_builder_host(callbacks) };
        // SAFETY: The element and context remain live for the duration of the call.
        let facts = unsafe { (host.callbacks.display_contents_facts)(host.callbacks.builder, element, context) };

        // A display:contents member builds its children through this path, so the top layer flag
        // is consumed here the same way update_layout_tree does for members with a box.
        let clear_layout_top_layer_for_descendants = facts.rendered_in_top_layer && facts.context_layout_top_layer;
        if clear_layout_top_layer_for_descendants {
            // SAFETY: `context` remains live throughout this call.
            unsafe { (host.callbacks.set_context_layout_top_layer)(context, false) };
        }

        // SAFETY: The builder and element remain live throughout this call.
        unsafe {
            (host.callbacks.clear_synthetic_pseudo_element_layout_nodes)(host.callbacks.builder, element);
        }

        if should_create_layout_node {
            // SAFETY: The builder and element remain live throughout this call.
            unsafe {
                (host.callbacks.clear_stale_inclusive_descendants)(host.callbacks.builder, element);
                (host.callbacks.resolve_counters)(element);
            }
        }

        if !facts.content_visibility_hidden {
            // SAFETY: The builder and element remain live throughout this call.
            unsafe {
                (host.callbacks.create_pseudo_element)(
                    host.callbacks.builder,
                    element,
                    FfiPseudoElement::Before,
                    FfiInsertionMode::Append,
                );
            }
        }

        if !facts.content_visibility_hidden && (should_create_layout_node || facts.child_needs_layout_tree_update) {
            let must_create_children = should_create_layout_node;
            if !facts.shadow_root.is_null() {
                // SAFETY: The callback table, shadow root, and context remain valid.
                unsafe {
                    update_layout_tree_for_shadow_root_children(
                        callbacks,
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
                        callbacks,
                        facts.dom_children_parent,
                        context,
                        must_create_children,
                    );
                }
            }
        }

        if !facts.slot_element.is_null() {
            if !facts.content_visibility_hidden {
                // SAFETY: The callback table, slot element, and context remain valid.
                unsafe {
                    update_layout_tree_for_assigned_slottables(
                        callbacks,
                        facts.slot_element,
                        context,
                        must_create_subtree,
                    );
                }
            } else {
                // SAFETY: `slot_element` remains live throughout this call.
                unsafe { (host.callbacks.clear_stale_assigned_slottables)(facts.slot_element) };
            }
        }

        if !facts.content_visibility_hidden {
            // SAFETY: The builder and element remain live throughout this call.
            unsafe {
                (host.callbacks.create_pseudo_element)(
                    host.callbacks.builder,
                    element,
                    FfiPseudoElement::After,
                    FfiInsertionMode::Append,
                );
            }
        }

        assert!(!facts.dom_children_parent.is_null());
        // SAFETY: The element's ParentNode subobject remains live throughout this call.
        unsafe { (host.callbacks.clear_update_flags)(facts.dom_children_parent) };

        if clear_layout_top_layer_for_descendants {
            // SAFETY: Restore the live traversal context to its entry value.
            unsafe { (host.callbacks.set_context_layout_top_layer)(context, true) };
        }
    });
}

fn ancestor_stack_contains_dom_node(host: &DomTreeBuilderHost<'_>, dom_node: *mut c_void) -> bool {
    // SAFETY: The builder and its ancestor stack remain live throughout tree construction.
    let count = unsafe { (host.callbacks.ancestor_count)(host.callbacks.builder) };
    for index in 0..count {
        // SAFETY: `index` is below the stable ancestor count and the returned layout node is live.
        let ancestor = unsafe { (host.callbacks.ancestor_at)(host.callbacks.builder, index) };
        assert!(!ancestor.is_null());
        // SAFETY: `ancestor` is a live layout node.
        if unsafe { (host.callbacks.layout_node_dom_node)(ancestor) } == dom_node {
            return true;
        }
    }
    false
}

fn update_svg_resource(
    host: &DomTreeBuilderHost<'_>,
    resource: *mut c_void,
    graphics_element: *mut c_void,
    layout_node: *mut c_void,
    context: *mut c_void,
    prior_context_value: bool,
) {
    // SAFETY: The traversal context and layout node remain live throughout resource construction.
    unsafe {
        (host.callbacks.set_context_layout_svg_mask_or_clip_path)(context, true);
        (host.callbacks.push_layout_parent)(host.callbacks.builder, layout_node);
    }

    if !ancestor_stack_contains_dom_node(host, resource) {
        host.update_layout_tree(resource, context, true);
        // SAFETY: Both pointers denote live SVG elements held by the graphics element.
        unsafe { (host.callbacks.register_svg_resource_reference)(resource, graphics_element) };
    } else {
        // FIXME: Somehow either remove ancestor from the layout tree or mark it as invalid.
    }

    // SAFETY: Balance the parent push and restore the context's entry value.
    unsafe {
        (host.callbacks.pop_layout_parent)(host.callbacks.builder);
        (host.callbacks.set_context_layout_svg_mask_or_clip_path)(context, prior_context_value);
    }
}

fn update_svg_pattern(
    host: &DomTreeBuilderHost<'_>,
    pattern: *mut c_void,
    content_element: *mut c_void,
    graphics_element: *mut c_void,
    layout_node: *mut c_void,
    context: *mut c_void,
    prior_context_value: bool,
) {
    // SAFETY: The traversal context and layout node remain live throughout resource construction.
    unsafe {
        (host.callbacks.set_context_layout_svg_pattern)(context, true);
        (host.callbacks.push_layout_parent)(host.callbacks.builder, layout_node);
    }

    if !ancestor_stack_contains_dom_node(host, content_element) {
        host.update_layout_tree(content_element, context, true);
        // The referenced pattern may inherit its content from another pattern via href. Removing either element
        // invalidates the attached resource box, so register the referencer with both.
        // SAFETY: All pointers denote live SVG elements held by the graphics element or pattern chain.
        unsafe {
            (host.callbacks.register_svg_resource_reference)(content_element, graphics_element);
            if pattern != content_element {
                (host.callbacks.register_svg_resource_reference)(pattern, graphics_element);
            }
        }
    }

    // SAFETY: Balance the parent push and restore the context's entry value.
    unsafe {
        (host.callbacks.pop_layout_parent)(host.callbacks.builder);
        (host.callbacks.set_context_layout_svg_pattern)(context, prior_context_value);
    }
}

/// Updates the descendants and post-child state of a node with a principal layout box.
///
/// # Safety
///
/// The callback table, DOM node, layout node, and context must remain valid for the duration of the call.
unsafe fn update_principal_node_descendants(
    callbacks: *const FfiDomTreeBuilderCallbacks,
    dom_node: *mut c_void,
    layout_node: *mut c_void,
    context: *mut c_void,
    should_create_layout_node: bool,
    must_create_subtree: bool,
) {
    abort_on_panic(|| {
        assert!(!dom_node.is_null());
        assert!(!layout_node.is_null());
        assert!(!context.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let host = unsafe { dom_tree_builder_host(callbacks) };
        // SAFETY: All pointers remain live throughout the call.
        let facts = unsafe {
            (host.callbacks.principal_descendant_facts)(host.callbacks.builder, dom_node, layout_node, context)
        };
        // SAFETY: The builder remains live throughout the call.
        let prior_quote_nesting_level = unsafe { (host.callbacks.quote_nesting_level)(host.callbacks.builder) };

        if should_create_layout_node {
            // Resolve counters now that we exist in the layout tree.
            if facts.is_element {
                // SAFETY: `dom_node` is a live Element when this fact is set.
                unsafe { (host.callbacks.resolve_counters)(dom_node) };
            }

            // Add the ::before pseudo-element before walking normal children.
            if facts.is_element && facts.layout_node_can_have_children && !facts.content_visibility_hidden {
                // SAFETY: The builder, element, and layout node remain live throughout pseudo-element creation.
                unsafe {
                    (host.callbacks.push_layout_parent)(host.callbacks.builder, layout_node);
                    (host.callbacks.create_pseudo_element)(
                        host.callbacks.builder,
                        dom_node,
                        FfiPseudoElement::Before,
                        FfiInsertionMode::Prepend,
                    );
                    (host.callbacks.pop_layout_parent)(host.callbacks.builder);
                }
            }
        }

        if facts.content_visibility_hidden {
            // SAFETY: The builder and DOM node remain live throughout the call.
            unsafe {
                (host.callbacks.clear_stale_descendants)(host.callbacks.builder, dom_node);
            }
        }

        if (should_create_layout_node || facts.child_needs_layout_tree_update)
            && (!facts.shadow_root.is_null() || facts.should_layout_dom_children)
            && facts.layout_node_can_have_children
            && !facts.content_visibility_hidden
        {
            // SAFETY: `layout_node` remains live throughout descendant construction.
            unsafe { (host.callbacks.push_layout_parent)(host.callbacks.builder, layout_node) };

            if !facts.shadow_root.is_null() {
                if facts.layout_node_is_replaced_box_with_children {
                    // For replaced elements with shadow DOM children, wrap the children in an
                    // anonymous BlockContainer so that a BFC handles their layout.
                    // SAFETY: The callback returns the live first-child wrapper.
                    let wrapper = unsafe {
                        (host.callbacks.ensure_replaced_children_wrapper)(host.callbacks.builder, layout_node)
                    };
                    assert!(!wrapper.is_null());
                    // SAFETY: `wrapper` remains live and attached throughout the recursive call.
                    unsafe { (host.callbacks.push_layout_parent)(host.callbacks.builder, wrapper) };
                }
                // SAFETY: The callback table, shadow root, and context remain valid.
                unsafe {
                    update_layout_tree_for_shadow_root_children(
                        callbacks,
                        facts.shadow_root,
                        context,
                        should_create_layout_node,
                    );
                }
                if facts.layout_node_is_replaced_box_with_children {
                    // SAFETY: Balances the wrapper push above.
                    unsafe { (host.callbacks.pop_layout_parent)(host.callbacks.builder) };
                }
            } else if facts.should_layout_dom_children {
                assert!(!facts.dom_children_parent.is_null());
                if facts.is_svg_switch_element {
                    // SAFETY: The callback table, parent, and context remain valid.
                    unsafe {
                        update_layout_tree_for_svg_switch_children(
                            callbacks,
                            facts.dom_children_parent,
                            context,
                            should_create_layout_node,
                        );
                    }
                } else {
                    // SAFETY: The callback table, parent, and context remain valid.
                    unsafe {
                        update_layout_tree_for_dom_children(
                            callbacks,
                            facts.dom_children_parent,
                            context,
                            should_create_layout_node,
                        );
                    }
                }
            }

            if facts.is_document {
                // Elements in the top layer do not lay out normally based on their position in the document; instead
                // they generate boxes as if they were siblings of the root element.
                // SAFETY: The traversal context remains live and is restored before this function returns.
                unsafe { (host.callbacks.set_context_layout_top_layer)(context, true) };
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
                    if has_unrendered_flat_tree_ancestor(&host, element) {
                        // SAFETY: The builder and element remain live throughout cleanup.
                        unsafe {
                            (host.callbacks.clear_stale_top_layer_subtree)(host.callbacks.builder, element);
                        }
                        continue;
                    }
                    host.update_layout_tree(element, context, should_create_layout_node);
                }
                // SAFETY: Restore the traversal context's entry value for the document walk.
                unsafe { (host.callbacks.set_context_layout_top_layer)(context, facts.context_layout_top_layer) };
            }

            // SAFETY: Balances the principal layout-node push above.
            unsafe { (host.callbacks.pop_layout_parent)(host.callbacks.builder) };
        }

        if !facts.slot_element.is_null() {
            if !facts.content_visibility_hidden {
                // SAFETY: `layout_node` remains live throughout assigned-node construction.
                unsafe { (host.callbacks.push_layout_parent)(host.callbacks.builder, layout_node) };
                // SAFETY: The callback table, slot element, and context remain valid.
                unsafe {
                    update_layout_tree_for_assigned_slottables(
                        callbacks,
                        facts.slot_element,
                        context,
                        must_create_subtree,
                    );
                }
                // SAFETY: Balances the assigned-node parent push above.
                unsafe { (host.callbacks.pop_layout_parent)(host.callbacks.builder) };
            } else {
                // SAFETY: `slot_element` remains live throughout cleanup.
                unsafe { (host.callbacks.clear_stale_assigned_slottables)(facts.slot_element) };
            }
        }

        if should_create_layout_node {
            if !facts.svg_graphics_element.is_null() {
                for resource in [facts.svg_mask, facts.svg_clip_path] {
                    if !resource.is_null() {
                        update_svg_resource(
                            &host,
                            resource,
                            facts.svg_graphics_element,
                            layout_node,
                            context,
                            facts.context_layout_svg_mask_or_clip_path,
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
                        &host,
                        pattern,
                        content_element,
                        facts.svg_graphics_element,
                        layout_node,
                        context,
                        facts.context_layout_svg_pattern,
                    );
                }
            }

            // Add ::marker and ::after once normal and SVG resource children are complete.
            if facts.is_element && facts.layout_node_can_have_children && !facts.content_visibility_hidden {
                // SAFETY: The builder, element, and layout node remain live throughout pseudo-element creation.
                unsafe {
                    (host.callbacks.push_layout_parent)(host.callbacks.builder, layout_node);
                    if facts.layout_node_is_list_item_box {
                        (host.callbacks.create_pseudo_element)(
                            host.callbacks.builder,
                            dom_node,
                            FfiPseudoElement::Marker,
                            FfiInsertionMode::Prepend,
                        );
                    }
                    (host.callbacks.create_pseudo_element)(
                        host.callbacks.builder,
                        dom_node,
                        FfiPseudoElement::After,
                        FfiInsertionMode::Append,
                    );
                    (host.callbacks.pop_layout_parent)(host.callbacks.builder);
                }

                if facts.layout_node_is_block_container && facts.has_first_letter_style {
                    // SAFETY: `dom_node` is an Element and `layout_node` is a BlockContainer when these facts are set.
                    unsafe {
                        (host.callbacks.create_first_letter_wrapper)(host.callbacks.builder, dom_node, layout_node);
                    }
                }
            }

            // SAFETY: All pointers remain live throughout the calls.
            unsafe {
                (host.callbacks.wrap_fieldset_layout)(host.callbacks.builder, layout_node);
                (host.callbacks.wrap_button_layout)(host.callbacks.builder, dom_node, layout_node);
            }
        }

        // https://www.w3.org/TR/css-contain-2/#containment-style
        // Giving an element style containment has the following effects:
        // 2. The effects of the 'content' property’s 'open-quote', 'close-quote', 'no-open-quote' and 'no-close-quote'
        //    must be scoped to the element’s sub-tree.
        if facts.has_style_containment {
            // SAFETY: The builder remains live throughout the call.
            unsafe {
                (host.callbacks.set_quote_nesting_level)(host.callbacks.builder, prior_quote_nesting_level);
            }
        }

        // SAFETY: `dom_node` remains live throughout the call.
        unsafe { (host.callbacks.clear_dom_update_flags)(dom_node) };
    });
}

struct PrincipalNodeUpdate<'host, 'callbacks> {
    host: &'host DomTreeBuilderHost<'callbacks>,
    callbacks: *const FfiDomTreeBuilderCallbacks,
    frame: *mut c_void,
    dom_node: *mut c_void,
    context: *mut c_void,
    must_create_subtree: bool,
}

fn construct_principal_layout_node(
    update: &PrincipalNodeUpdate<'_, '_>,
    entry_facts: FfiPrincipalNodeEntryFacts,
    should_create_layout_node: bool,
) -> (bool, bool) {
    let host = update.host;
    let frame = update.frame;
    let dom_node = update.dom_node;
    let context = update.context;
    if entry_facts.is_element {
        // SAFETY: The frame, builder, and DOM element remain live throughout the call.
        let display = unsafe {
            (host.callbacks.prepare_principal_element)(
                host.callbacks.builder,
                frame,
                dom_node,
                should_create_layout_node,
            )
        };
        let generation = principal_box_generation_decision(
            true,
            should_create_layout_node && display.display_is_none,
            display.display_is_contents,
        );
        if generation == PrincipalBoxGenerationDecision::Suppress {
            return (false, false);
        }
        if generation == PrincipalBoxGenerationDecision::DisplayContents {
            // SAFETY: The callback table, DOM element, and context remain valid throughout the recursive walk.
            unsafe {
                update_layout_tree_for_display_contents(
                    update.callbacks,
                    dom_node,
                    context,
                    update.must_create_subtree,
                    should_create_layout_node,
                );
            }
            return (false, true);
        }
        if should_create_layout_node {
            // SAFETY: The frame, element, and context remain live throughout construction.
            let layout_facts = unsafe { (host.callbacks.principal_element_layout_facts)(frame, dom_node, context) };
            let layout_kind = element_layout_kind(layout_facts);
            // SAFETY: The builder, frame, and element remain live throughout construction.
            unsafe {
                (host.callbacks.create_principal_element_layout)(host.callbacks.builder, frame, dom_node, layout_kind);
            }
            if matches!(
                layout_kind,
                FfiElementLayoutKind::SvgMask | FfiElementLayoutKind::SvgClipPath
            ) {
                // Only direct mask and clip-path uses inherit this construction mode.
                // SAFETY: The traversal context remains live throughout the call.
                unsafe { (host.callbacks.set_context_layout_svg_mask_or_clip_path)(context, false) };
            } else if layout_kind == FfiElementLayoutKind::SvgPattern {
                // Only the directly referenced pattern inherits this construction mode.
                // SAFETY: The traversal context remains live throughout the call.
                unsafe { (host.callbacks.set_context_layout_svg_pattern)(context, false) };
            }
        } else {
            // SAFETY: The frame and DOM node remain live throughout the call.
            unsafe { (host.callbacks.reuse_principal_layout)(frame, dom_node) };
        }
    } else if should_create_layout_node {
        if entry_facts.is_document {
            // SAFETY: The frame and DOM document remain live throughout construction.
            unsafe { (host.callbacks.create_principal_document_layout)(frame, dom_node) };
        } else if entry_facts.is_text {
            // SAFETY: The frame and DOM text node remain live throughout construction.
            unsafe { (host.callbacks.create_principal_text_layout)(frame, dom_node) };
        }
    } else {
        // SAFETY: The frame and DOM node remain live throughout the call.
        unsafe { (host.callbacks.reuse_principal_layout)(frame, dom_node) };
    }

    // SAFETY: The frame remains live throughout the call.
    (
        !unsafe { (host.callbacks.principal_layout_node)(frame) }.is_null(),
        false,
    )
}

fn update_principal_node_after_entry(
    update: &PrincipalNodeUpdate<'_, '_>,
    entry_facts: FfiPrincipalNodeEntryFacts,
    entry_decision: PrincipalNodeEntryDecision,
) {
    let host = update.host;
    let frame = update.frame;
    let dom_node = update.dom_node;
    let context = update.context;
    // SAFETY: The frame and DOM node remain live throughout this update.
    unsafe { (host.callbacks.initialize_principal_frame)(frame, dom_node) };

    if entry_decision.svg == SvgEntryDecision::EnterSvgRoot {
        // SAFETY: The traversal context remains live throughout this update.
        unsafe { (host.callbacks.set_context_has_svg_root)(context, true) };
    }

    let (has_layout_node, handled_display_contents) = if entry_decision.svg == SvgEntryDecision::Skip {
        (false, false)
    } else {
        construct_principal_layout_node(update, entry_facts, entry_decision.should_create_layout_node)
    };

    if has_layout_node {
        if entry_facts.is_element || entry_facts.is_document {
            // SAFETY: The frame owns a live NodeWithStyle for elements and documents.
            unsafe { (host.callbacks.attach_principal_style_resources)(frame) };
        }

        // SAFETY: The frame owns the live principal layout node and its display.
        let layout_facts = unsafe { (host.callbacks.principal_layout_facts)(frame) };
        if layout_facts.is_replaced_element {
            let adjustment = adjusted_table_display_for_replaced_element(
                layout_facts.display.display_is_table_inside,
                layout_facts.display.display_is_block_outside,
                layout_facts.display.display_is_internal_table,
                layout_facts.display.display_is_table_caption,
            );
            if adjustment != FfiReplacedElementDisplayAdjustment::None {
                // SAFETY: The frame owns a live NodeWithStyle.
                unsafe { (host.callbacks.apply_replaced_display_adjustment)(frame, adjustment) };
            }
        }

        // SAFETY: All pointers remain live and the frame owns both old and new layout nodes.
        let placement_facts = unsafe {
            (host.callbacks.principal_placement_facts)(
                host.callbacks.builder,
                frame,
                dom_node,
                context,
                update.must_create_subtree,
                entry_decision.should_create_layout_node,
            )
        };
        let placement = principal_box_placement_decision(placement_facts);

        let mut prior_rebuild_root = std::ptr::null_mut();
        if placement.start_rebuild_root {
            // SAFETY: The builder and frame remain live throughout descendant construction.
            prior_rebuild_root =
                unsafe { (host.callbacks.start_principal_rebuild_root)(host.callbacks.builder, frame) };
        } else if placement.mark_update_escaped_rebuild_roots {
            // SAFETY: The builder remains live throughout the call.
            unsafe { (host.callbacks.mark_update_escaped_rebuild_roots)(host.callbacks.builder) };
        }

        if placement.create_backdrop {
            // A backdrop is a sibling of its originating top-layer element. Append it normally, but insert it before
            // an old box that will be replaced in place so the backdrop remains behind the element.
            // SAFETY: The builder and DOM element remain live throughout pseudo-element construction.
            let backdrop = unsafe {
                (host.callbacks.create_principal_backdrop)(
                    host.callbacks.builder,
                    frame,
                    dom_node,
                    !placement.may_replace_existing_layout_node,
                )
            };
            if placement.may_replace_existing_layout_node && !backdrop.is_null() {
                // SAFETY: The frame retains the attached old layout node and `backdrop` is owned by the element.
                unsafe {
                    (host.callbacks.insert_principal_backdrop_before_old)(host.callbacks.builder, frame, backdrop);
                }
            }
        }

        if placement.clear_layout_top_layer_for_descendants {
            // SAFETY: The traversal context remains live throughout descendant construction.
            unsafe { (host.callbacks.set_context_layout_top_layer)(context, false) };
        }

        // SAFETY: The builder and frame remain live, and Rust selected the placement mode.
        unsafe { (host.callbacks.place_principal_layout)(host.callbacks.builder, frame, placement.placement) };
        // SAFETY: The callback table, DOM node, layout node, and context remain live throughout the call.
        unsafe {
            update_principal_node_descendants(
                update.callbacks,
                dom_node,
                (host.callbacks.principal_layout_node)(frame),
                context,
                entry_decision.should_create_layout_node,
                update.must_create_subtree,
            );
        }

        if placement.clear_layout_top_layer_for_descendants {
            // SAFETY: Restore the context value captured in the placement facts.
            unsafe { (host.callbacks.set_context_layout_top_layer)(context, placement_facts.context_layout_top_layer) };
        }
        if placement.start_rebuild_root {
            // SAFETY: Restore the builder's previous rebuild root after descendant construction.
            unsafe { (host.callbacks.restore_principal_rebuild_root)(host.callbacks.builder, prior_rebuild_root) };
        }
    } else if !handled_display_contents {
        // If no layout node was created, remove every stale layout and paint node from the shadow-including subtree.
        // SAFETY: The builder and DOM node remain live throughout cleanup.
        unsafe { (host.callbacks.clear_stale_inclusive_subtree)(host.callbacks.builder, dom_node) };
    }

    if entry_decision.svg == SvgEntryDecision::EnterSvgRoot {
        // SAFETY: Restore the traversal context's entry value.
        unsafe { (host.callbacks.set_context_has_svg_root)(context, entry_facts.context_has_svg_root) };
    }
}

/// Updates one DOM node and its layout-tree subtree.
///
/// # Safety
///
/// The callback table, frame storage, DOM node, and context must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_update_layout_tree(
    callbacks: *const FfiDomTreeBuilderCallbacks,
    frame: *mut c_void,
    dom_node: *mut c_void,
    context: *mut c_void,
    must_create_subtree: bool,
) {
    abort_on_panic(|| {
        assert!(!frame.is_null());
        assert!(!dom_node.is_null());
        assert!(!context.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let host = unsafe { dom_tree_builder_host(callbacks) };
        // SAFETY: The builder, DOM node, and context remain live throughout the call.
        let entry_facts = unsafe {
            (host.callbacks.principal_node_entry_facts)(host.callbacks.builder, dom_node, context, must_create_subtree)
        };
        let entry_decision = principal_node_entry_decision(entry_facts);
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

        if entry_facts.is_element {
            // SAFETY: `dom_node` is a live Element when this fact is set.
            unsafe { (host.callbacks.push_style_ancestor)(dom_node) };
        }
        let update = PrincipalNodeUpdate {
            host: &host,
            callbacks,
            frame,
            dom_node,
            context,
            must_create_subtree,
        };
        update_principal_node_after_entry(&update, entry_facts, entry_decision);
        if entry_facts.is_element {
            // SAFETY: Balances the style ancestor push above.
            unsafe { (host.callbacks.pop_style_ancestor)(dom_node) };
        }
    });
}

/// Builds or incrementally updates a document's layout tree and applies table fixup.
///
/// # Safety
///
/// The callback table, frame storage, document, and context must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_build_layout_tree(
    callbacks: *const FfiDomTreeBuilderCallbacks,
    frame: *mut c_void,
    document: *mut c_void,
    context: *mut c_void,
) -> *mut c_void {
    abort_on_panic(|| {
        assert!(!frame.is_null());
        assert!(!document.is_null());
        assert!(!context.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let host = unsafe { dom_tree_builder_host(callbacks) };
        // SAFETY: All pointers remain live throughout the build.
        let entry_facts =
            unsafe { (host.callbacks.principal_node_entry_facts)(host.callbacks.builder, document, context, false) };
        assert!(entry_facts.is_document);

        // SAFETY: The document and builder remain live throughout the build.
        unsafe {
            (host.callbacks.reset_style_ancestor_filter)(document);
            (host.callbacks.set_quote_nesting_level)(host.callbacks.builder, 0);
            rust_update_layout_tree(callbacks, frame, document, context, false);
        }

        // NB: Called during layout tree construction.
        // SAFETY: The document remains live and any attached layout root is owned by it and the builder.
        let document_layout_node = unsafe { (host.callbacks.document_layout_node)(document) };
        if !document_layout_node.is_null() {
            // SAFETY: The builder and layout root remain live throughout table fixup.
            unsafe { (host.callbacks.fixup_tables)(host.callbacks.builder, document_layout_node) };
        }

        // SAFETY: The builder remains live and owns the returned root.
        unsafe { (host.callbacks.layout_root)(host.callbacks.builder) }
    })
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
    NormalMarker,
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
    pub initialize: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiPseudoElement) -> FfiPseudoElementFacts,
    pub create_layout_node:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, FfiPseudoElement, FfiPseudoElementDecision),
    pub layout_node: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    pub attach_style_resources: unsafe extern "C" fn(*mut c_void),
    pub layout_facts: unsafe extern "C" fn(*mut c_void) -> FfiPrincipalLayoutFacts,
    pub apply_replaced_display_adjustment: unsafe extern "C" fn(*mut c_void, FfiReplacedElementDisplayAdjustment),
    pub layout_node_is_list_item: unsafe extern "C" fn(*mut c_void) -> bool,
    pub create_nested_list_marker: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub configure_layout_node: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiPseudoElement, u32),
    pub insert_layout_node: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiInsertionMode),
    pub resolve_counters: unsafe extern "C" fn(*mut c_void, FfiPseudoElement),
    pub resolve_content:
        unsafe extern "C" fn(*mut c_void, *mut c_void, FfiPseudoElement, u32) -> FfiResolvedPseudoContentFacts,
    pub create_content_item: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiPseudoElement, usize) -> *mut c_void,
    pub insert_content_item: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub push_layout_parent: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub pop_layout_parent: unsafe extern "C" fn(*mut c_void),
    pub quote_nesting_level: unsafe extern "C" fn(*mut c_void) -> u32,
    pub set_quote_nesting_level: unsafe extern "C" fn(*mut c_void, u32),
}

fn pseudo_element_decision(facts: FfiPseudoElementFacts) -> FfiPseudoElementDecision {
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
                FfiPseudoElementDecision::NormalMarker
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

/// Creates a generated pseudo-element layout subtree when its style and content require one.
///
/// # Safety
///
/// The callback table, frame storage, and originating element must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_create_pseudo_element(
    callbacks: *const FfiPseudoTreeBuilderCallbacks,
    frame: *mut c_void,
    element: *mut c_void,
    pseudo_element: FfiPseudoElement,
    has_insertion_mode: bool,
    insertion_mode: FfiInsertionMode,
) -> *mut c_void {
    abort_on_panic(|| {
        assert!(!callbacks.is_null());
        assert!(!frame.is_null());
        assert!(!element.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let callbacks = unsafe { &*callbacks };
        // SAFETY: The frame and element remain live throughout initialization.
        let facts = unsafe { (callbacks.initialize)(frame, element, pseudo_element) };
        let decision = pseudo_element_decision(facts);
        if decision == FfiPseudoElementDecision::None {
            return std::ptr::null_mut();
        }

        // SAFETY: The builder, frame, and element remain live throughout construction.
        unsafe {
            (callbacks.create_layout_node)(callbacks.builder, frame, element, pseudo_element, decision);
        }
        // SAFETY: The frame remains live throughout the call.
        let layout_node = unsafe { (callbacks.layout_node)(frame) };
        if layout_node.is_null() || decision == FfiPseudoElementDecision::NormalMarker {
            return layout_node;
        }

        // SAFETY: The frame owns a live pseudo-element layout node.
        unsafe { (callbacks.attach_style_resources)(frame) };
        if decision == FfiPseudoElementDecision::ContentReplacement {
            // SAFETY: The frame owns the live replacement node and its display.
            let layout_facts = unsafe { (callbacks.layout_facts)(frame) };
            let adjustment = adjusted_table_display_for_replaced_element(
                layout_facts.display.display_is_table_inside,
                layout_facts.display.display_is_block_outside,
                layout_facts.display.display_is_internal_table,
                layout_facts.display.display_is_table_caption,
            );
            if adjustment != FfiReplacedElementDisplayAdjustment::None {
                // SAFETY: The frame owns a live NodeWithStyle.
                unsafe { (callbacks.apply_replaced_display_adjustment)(frame, adjustment) };
            }
        }

        // FIXME: This code actually computes style for element::marker, and shouldn't for element::pseudo::marker.
        // SAFETY: The frame owns a live pseudo-element layout node.
        if unsafe { (callbacks.layout_node_is_list_item)(frame) } {
            // SAFETY: The builder, frame, and element remain live throughout marker creation.
            unsafe { (callbacks.create_nested_list_marker)(callbacks.builder, frame, element) };
            // FIXME: Support counters on element::pseudo::marker.
        }

        // SAFETY: The builder and frame remain live throughout configuration and insertion.
        let initial_quote_nesting_level = unsafe { (callbacks.quote_nesting_level)(callbacks.builder) };
        unsafe {
            (callbacks.configure_layout_node)(frame, element, pseudo_element, initial_quote_nesting_level);
            if has_insertion_mode {
                (callbacks.insert_layout_node)(callbacks.builder, frame, insertion_mode);
            }
            (callbacks.resolve_counters)(element, pseudo_element);
        }

        // Resolve content after insertion because counter() and counters() items read the counters established by this
        // pseudo-element's box.
        // SAFETY: The frame and element remain live throughout content resolution.
        let resolved_content =
            unsafe { (callbacks.resolve_content)(frame, element, pseudo_element, initial_quote_nesting_level) };
        // SAFETY: The builder remains live throughout the call.
        unsafe {
            (callbacks.set_quote_nesting_level)(callbacks.builder, resolved_content.final_quote_nesting_level);
        }

        if resolved_content.content_is_list && decision != FfiPseudoElementDecision::ContentReplacement {
            // SAFETY: The pseudo-element node remains live throughout child construction.
            unsafe { (callbacks.push_layout_parent)(callbacks.builder, layout_node) };
            for index in 0..resolved_content.content_item_count {
                // SAFETY: `index` is below the resolved content item count and the frame retains the returned node.
                let content_item = unsafe { (callbacks.create_content_item)(frame, element, pseudo_element, index) };
                assert!(!content_item.is_null());
                // SAFETY: The builder and generated content item remain live throughout insertion.
                unsafe { (callbacks.insert_content_item)(callbacks.builder, content_item) };
            }
            // SAFETY: Balances the pseudo-element parent push above.
            unsafe { (callbacks.pop_layout_parent)(callbacks.builder) };
        }

        layout_node
    })
}

fn adjusted_table_display_for_replaced_element(
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// NB: Some variants are only constructed by C++ through the FFI.
#[allow(dead_code)]
pub enum FfiTableDisplay {
    Other,
    TableRoot,
    TableRowGroup,
    TableHeaderGroup,
    TableFooterGroup,
    TableColumnGroup,
    TableColumn,
    TableRow,
    TableCell,
    TableCaption,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutNodeFacts {
    pub current_display: FfiTableDisplay,
    pub display_before_box_type_transformation: FfiTableDisplay,
    pub is_box: bool,
    pub has_style: bool,
    pub is_anonymous: bool,
    pub is_block_container: bool,
    pub children_are_inline: bool,
    pub is_out_of_flow: bool,
    pub is_text: bool,
    pub text_is_ascii_whitespace: bool,
    pub is_inline_outside: bool,
    pub is_table_wrapper: bool,
    pub has_been_wrapped_in_table_wrapper: bool,
    pub has_replaced_element_table_display_adjustment: bool,
    pub is_inline: bool,
    pub is_in_flow: bool,
    pub is_inline_node: bool,
    pub is_field_set_box: bool,
    pub is_svg_foreign_object_box: bool,
    pub is_svg_box: bool,
    pub is_svg_svg_box: bool,
    pub is_generated_for_pseudo_element: bool,
    pub display_is_flow_inside: bool,
    pub display_is_flex_inside: bool,
    pub display_is_grid_inside: bool,
    pub is_list_item_marker_box: bool,
    pub is_generated_for_marker: bool,
    pub is_fragmented_inline: bool,
    pub has_first_letter_style: bool,
    pub has_rendered_legend: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiFirstLetterTarget {
    pub text_node: *mut c_void,
    pub letter_start: usize,
    pub letter_end: usize,
    pub found: bool,
}

impl FfiFirstLetterTarget {
    fn not_found() -> Self {
        Self {
            text_node: std::ptr::null_mut(),
            letter_start: 0,
            letter_end: 0,
            found: false,
        }
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiFirstLetterCodePointFacts {
    pub is_space_separator: bool,
    pub is_punctuation: bool,
    pub is_letter: bool,
    pub is_number: bool,
    pub is_symbol: bool,
    pub is_open_punctuation: bool,
    pub is_dash_punctuation: bool,
}

#[repr(C)]
pub struct FfiFirstLetterTextCallbacks {
    pub context: *mut c_void,
    pub code_unit_length: unsafe extern "C" fn(*mut c_void) -> usize,
    pub code_point_at: unsafe extern "C" fn(*mut c_void, usize) -> u32,
    pub next_grapheme_boundary: unsafe extern "C" fn(*mut c_void, usize) -> usize,
    pub code_point_facts: unsafe extern "C" fn(*mut c_void, u32) -> FfiFirstLetterCodePointFacts,
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
#[repr(u8)]
// NB: `Prepend` is constructed by C++ through the FFI.
#[allow(dead_code)]
pub enum FfiInsertionMode {
    Append,
    Prepend,
}

#[repr(C)]
pub struct FfiTreeBuilderCallbacks {
    pub context: *mut c_void,
    pub layout_node_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiLayoutNodeFacts,
    pub parent: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub first_child: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub next_sibling: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub previous_sibling: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub last_child: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub remove_nodes: unsafe extern "C" fn(*mut c_void, *const *mut c_void, usize),
    pub wrap_in_anonymous:
        unsafe extern "C" fn(*mut c_void, *const *mut c_void, usize, *mut c_void, FfiAnonymousTableBoxKind),
    pub update_existing_table_wrapper: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub wrap_table_root: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub create_table_grid: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub destroy_table_grid: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub table_grid_column_count: unsafe extern "C" fn(*mut c_void, *mut c_void) -> usize,
    pub table_grid_is_occupied: unsafe extern "C" fn(*mut c_void, *mut c_void, usize, usize) -> bool,
    pub append_missing_table_cell: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub create_and_append_anonymous_wrapper: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub wrap_children_in_anonymous: unsafe extern "C" fn(*mut c_void, *mut c_void, *const *mut c_void, usize),
    pub insert_child: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, FfiInsertionMode),
    pub set_children_are_inline: unsafe extern "C" fn(*mut c_void, *mut c_void, bool),
    pub note_tree_restructuring: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub find_first_letter_in_text: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiFirstLetterTarget,
    pub create_button_content_wrapper: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub rendered_legend: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub create_fieldset_content_wrapper: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub move_nodes_to_parent: unsafe extern "C" fn(*mut c_void, *mut c_void, *const *mut c_void, usize),
}

type LayoutNode = *mut c_void;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TraversalDecision {
    Continue,
    SkipChildrenAndContinue,
    Break,
}

struct TreeBuilderHost<'a> {
    callbacks: &'a FfiTreeBuilderCallbacks,
}

impl TreeBuilderHost<'_> {
    fn facts(&self, node: LayoutNode) -> FfiLayoutNodeFacts {
        assert!(!node.is_null());
        // SAFETY: The entry point's callback contract guarantees that every node passed to a callback is live.
        unsafe { (self.callbacks.layout_node_facts)(self.callbacks.context, node) }
    }

    fn parent(&self, node: LayoutNode) -> LayoutNode {
        // SAFETY: The entry point's callback contract guarantees that `node` is live.
        unsafe { (self.callbacks.parent)(self.callbacks.context, node) }
    }

    fn first_child(&self, node: LayoutNode) -> LayoutNode {
        // SAFETY: The entry point's callback contract guarantees that `node` is live.
        unsafe { (self.callbacks.first_child)(self.callbacks.context, node) }
    }

    fn next_sibling(&self, node: LayoutNode) -> LayoutNode {
        // SAFETY: The entry point's callback contract guarantees that `node` is live.
        unsafe { (self.callbacks.next_sibling)(self.callbacks.context, node) }
    }

    fn previous_sibling(&self, node: LayoutNode) -> LayoutNode {
        // SAFETY: The entry point's callback contract guarantees that `node` is live.
        unsafe { (self.callbacks.previous_sibling)(self.callbacks.context, node) }
    }

    fn last_child(&self, node: LayoutNode) -> LayoutNode {
        // SAFETY: The entry point's callback contract guarantees that `node` is live.
        unsafe { (self.callbacks.last_child)(self.callbacks.context, node) }
    }

    fn for_each_in_inclusive_subtree(
        &self,
        root: LayoutNode,
        mut callback: impl FnMut(LayoutNode) -> TraversalDecision,
    ) {
        let mut current = root;
        while !current.is_null() {
            let decision = callback(current);
            if decision == TraversalDecision::Break {
                return;
            }

            if decision != TraversalDecision::SkipChildrenAndContinue {
                let first_child = self.first_child(current);
                if !first_child.is_null() {
                    current = first_child;
                    continue;
                }
            }
            if current == root {
                break;
            }

            let next_sibling = self.next_sibling(current);
            if !next_sibling.is_null() {
                current = next_sibling;
                continue;
            }

            while current != root && self.next_sibling(current).is_null() {
                current = self.parent(current);
            }
            if current == root {
                break;
            }

            current = self.next_sibling(current);
        }
    }

    fn remove_nodes(&self, nodes: &[LayoutNode]) {
        // SAFETY: All nodes remain tree-owned until the callback first retains the complete slice.
        unsafe { (self.callbacks.remove_nodes)(self.callbacks.context, nodes.as_ptr(), nodes.len()) };
    }

    fn wrap_in_anonymous(&self, nodes: &[LayoutNode], nearest_sibling: LayoutNode, kind: FfiAnonymousTableBoxKind) {
        assert!(!nodes.is_empty());
        // SAFETY: The nodes are live siblings and `nearest_sibling` is either null or their live next sibling.
        unsafe {
            (self.callbacks.wrap_in_anonymous)(
                self.callbacks.context,
                nodes.as_ptr(),
                nodes.len(),
                nearest_sibling,
                kind,
            );
        };
    }
}

fn has_inline_or_in_flow_block_children(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    let mut child = host.first_child(node);
    while !child.is_null() {
        let facts = host.facts(child);
        if facts.is_inline || facts.is_in_flow {
            return true;
        }
        child = host.next_sibling(child);
    }
    false
}

fn has_in_flow_block_children(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    if host.facts(node).children_are_inline {
        return false;
    }
    let mut child = host.first_child(node);
    while !child.is_null() {
        let facts = host.facts(child);
        if !facts.is_inline && facts.is_in_flow {
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
    let parent_facts = host.facts(parent);
    let child_facts = host.facts(child);
    parent_facts.current_display == FfiTableDisplay::TableRoot
        && child_facts.has_style
        && !child_facts.is_anonymous
        && child_facts.is_out_of_flow
        && !child_facts.has_replaced_element_table_display_adjustment
        && is_table_non_root_box_with_display(child_facts.display_before_box_type_transformation)
}

fn create_anonymous_wrapper(host: &TreeBuilderHost<'_>, parent: LayoutNode) -> LayoutNode {
    // SAFETY: `parent` is a live NodeWithStyle. The callback appends and returns a live anonymous wrapper.
    let wrapper = unsafe { (host.callbacks.create_and_append_anonymous_wrapper)(host.callbacks.context, parent) };
    assert!(!wrapper.is_null());
    wrapper
}

fn last_child_creating_anonymous_wrapper_if_needed(host: &TreeBuilderHost<'_>, parent: LayoutNode) -> LayoutNode {
    let last_child = host.last_child(parent);
    if last_child.is_null() {
        return create_anonymous_wrapper(host, parent);
    }
    let facts = host.facts(last_child);
    if !facts.is_anonymous || !facts.children_are_inline || facts.is_generated_for_pseudo_element {
        return create_anonymous_wrapper(host, parent);
    }
    last_child
}

// The insertion_parent_for_*() functions maintain the invariant that the in-flow children of
// block-level boxes must be either all block-level or all inline-level.
fn insertion_parent_for_inline_node(host: &TreeBuilderHost<'_>, parent: LayoutNode) -> LayoutNode {
    let facts = host.facts(parent);
    if facts.is_field_set_box || facts.is_svg_foreign_object_box {
        return last_child_creating_anonymous_wrapper_if_needed(host, parent);
    }

    // SVG layout ignores the inline/block distinction, and an anonymous wrapper would only hide
    // the child from SVGFormattingContext (e.g. a shape with a foreignObject sibling).
    if facts.is_svg_box || facts.is_svg_svg_box {
        return parent;
    }

    if facts.is_inline && facts.display_is_flow_inside {
        return parent;
    }

    if facts.display_is_flex_inside || facts.display_is_grid_inside {
        return last_child_creating_anonymous_wrapper_if_needed(host, parent);
    }

    if !has_in_flow_block_children(host, parent) || facts.children_are_inline {
        return parent;
    }

    // Parent has block-level children, insert into an anonymous wrapper block (and create it first if needed)
    last_child_creating_anonymous_wrapper_if_needed(host, parent)
}

fn insertion_parent_for_block_node(
    host: &TreeBuilderHost<'_>,
    parent: LayoutNode,
    node: LayoutNode,
    mode: FfiInsertionMode,
) -> LayoutNode {
    let parent_facts = host.facts(parent);

    // Inline is fine for in-flow block children (interrupting blocks) and for out-of-flow children;
    // the inline formatting context emits items for both.
    if !host.facts(node).is_anonymous && parent_facts.is_inline && parent_facts.display_is_flow_inside {
        return parent;
    }

    // SVG layout ignores the inline/block distinction; wrapping existing inline-level siblings
    // (e.g. shapes next to a foreignObject) would only hide them from SVGFormattingContext.
    if parent_facts.is_svg_box || parent_facts.is_svg_svg_box {
        return parent;
    }

    // Make sure we're not inserting into an inline node, since those do not support block nodes.
    let mut new_parent = parent;
    while host.facts(new_parent).is_inline_node {
        new_parent = host.parent(new_parent);
        assert!(!new_parent.is_null());
    }

    // If the parent block has no children, insert this block into parent.
    if !has_inline_or_in_flow_block_children(host, new_parent) {
        return new_parent;
    }

    // Table-internal boxes may have been blockified before insertion, but table fixup still needs to see them as
    // direct table children instead of grouping them with neighboring table whitespace.
    if is_out_of_flow_table_internal_child_of_table_root(host, new_parent, node) {
        return new_parent;
    }

    let node_facts = host.facts(node);
    let new_parent_facts = host.facts(new_parent);

    // If the block is out-of-flow,
    if node_facts.is_out_of_flow {
        let last_child = host.last_child(new_parent);
        assert!(!last_child.is_null());
        let last_child_facts = host.facts(last_child);

        // And we're appending while the parent's last child is an anonymous block, join that
        // anonymous block. Prepended boxes (e.g. an absolutely positioned ::before) belong at the
        // very start of the parent, not at the start of its trailing inline run.
        if mode == FfiInsertionMode::Append
            && !new_parent_facts.display_is_flex_inside
            && !new_parent_facts.display_is_grid_inside
            && !last_child_facts.is_generated_for_pseudo_element
            && last_child_facts.is_anonymous
            && last_child_facts.children_are_inline
        {
            return last_child;
        }

        // Otherwise, insert this block into parent.
        return new_parent;
    }

    // If the parent block has block-level children, insert this block into parent.
    if !new_parent_facts.children_are_inline {
        return new_parent;
    }

    // Parent block has inline-level children (our siblings); wrap these siblings into an anonymous wrapper block.
    // SAFETY: `new_parent` is live for the duration of this call.
    unsafe { (host.callbacks.note_tree_restructuring)(host.callbacks.context, new_parent) };
    let mut children_to_wrap = Vec::new();
    let mut child = host.first_child(new_parent);
    while !child.is_null() {
        if !is_out_of_flow_table_internal_child_of_table_root(host, new_parent, child) {
            children_to_wrap.push(child);
        }
        child = host.next_sibling(child);
    }
    // SAFETY: The callback retains the children before moving them and leaves `new_parent` live.
    unsafe {
        (host.callbacks.wrap_children_in_anonymous)(
            host.callbacks.context,
            new_parent,
            children_to_wrap.as_ptr(),
            children_to_wrap.len(),
        );
    };

    // Then it's safe to insert this block into parent.
    new_parent
}

/// Inserts a layout node while maintaining the inline/block child invariant.
///
/// # Safety
///
/// `callbacks`, `nearest_insertion_ancestor`, and `node` must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_insert_node_into_inline_or_block_ancestor(
    callbacks: *const FfiTreeBuilderCallbacks,
    nearest_insertion_ancestor: *mut c_void,
    node: *mut c_void,
    is_inline_outside: bool,
    mode: FfiInsertionMode,
) {
    abort_on_panic(|| {
        assert!(!callbacks.is_null());
        assert!(!nearest_insertion_ancestor.is_null());
        assert!(!node.is_null());
        // SAFETY: The caller guarantees that `callbacks` remains valid for the duration of this call.
        let host = TreeBuilderHost {
            callbacks: unsafe { &*callbacks },
        };

        let insertion_point = if is_inline_outside {
            insertion_parent_for_inline_node(&host, nearest_insertion_ancestor)
        } else {
            insertion_parent_for_block_node(&host, nearest_insertion_ancestor, node, mode)
        };

        // Insertion parents can be above the subtree being rebuilt in place: inline ancestors are
        // skipped, and out-of-flow boxes can join a trailing anonymous sibling.
        // SAFETY: `insertion_point` is live for the duration of this call.
        unsafe { (host.callbacks.note_tree_restructuring)(host.callbacks.context, insertion_point) };
        // SAFETY: The callback retains `node` while inserting it into the live insertion point.
        unsafe { (host.callbacks.insert_child)(host.callbacks.context, insertion_point, node, mode) };

        if is_inline_outside {
            // After inserting an inline-level box into a parent, mark the parent as having inline children.
            // SAFETY: `insertion_point` remains live and attached.
            unsafe { (host.callbacks.set_children_are_inline)(host.callbacks.context, insertion_point, true) };
        } else if host.facts(node).is_in_flow {
            let insertion_point_facts = host.facts(insertion_point);
            // Inline-flow parents keep their inline children flag; their IFC may contain interrupting blocks.
            if !insertion_point_facts.is_inline || !insertion_point_facts.display_is_flow_inside {
                // SAFETY: `insertion_point` remains live and attached.
                unsafe { (host.callbacks.set_children_are_inline)(host.callbacks.context, insertion_point, false) };
            }
        }
    });
}

struct FirstLetterTextHost<'a> {
    callbacks: &'a FfiFirstLetterTextCallbacks,
}

impl FirstLetterTextHost<'_> {
    fn code_unit_length(&self) -> usize {
        // SAFETY: The entry point's callback contract guarantees that the text context is live.
        unsafe { (self.callbacks.code_unit_length)(self.callbacks.context) }
    }

    fn code_point_at(&self, index: usize) -> u32 {
        // SAFETY: Callers only pass indices below `code_unit_length`.
        unsafe { (self.callbacks.code_point_at)(self.callbacks.context, index) }
    }

    fn next_grapheme_boundary(&self, index: usize) -> usize {
        // SAFETY: Callers only pass indices at or below `code_unit_length`.
        unsafe { (self.callbacks.next_grapheme_boundary)(self.callbacks.context, index) }
    }

    fn code_point_facts(&self, code_point: u32) -> FfiFirstLetterCodePointFacts {
        // SAFETY: The callback accepts every Unicode scalar value and lone surrogate value supplied by the text view.
        unsafe { (self.callbacks.code_point_facts)(self.callbacks.context, code_point) }
    }
}

fn code_unit_length_for_code_point(code_point: u32) -> usize {
    if code_point > 0xffff { 2 } else { 1 }
}

// https://drafts.csswg.org/css-pseudo-4/#first-letter-pattern
fn find_first_letter_in_text(host: &FirstLetterTextHost<'_>, preserves_segment_breaks: bool) -> FfiFirstLetterTarget {
    // NB: Matches the first-letter text pattern: (P (Zs|P)*)? (L|N|S) ((Zs|P-(Ps|Pd))* (P-(Ps|Pd))?)?

    let code_units = host.code_unit_length();
    let mut match_start = 0;
    while match_start < code_units {
        let mut cursor = match_start;
        let starting_code_point = host.code_point_at(cursor);

        // When white-space preserves segment breaks, a newline before any letter puts the letter on a later line, so
        // the first formatted line is empty and ::first-letter must not match.
        if preserves_segment_breaks && (starting_code_point == b'\n' as u32 || starting_code_point == b'\r' as u32) {
            return FfiFirstLetterTarget::not_found();
        }

        let starting_facts = host.code_point_facts(starting_code_point);

        // A valid match starts with either a P, or the letter itself.
        let has_preceding = starting_facts.is_punctuation;
        if !(has_preceding || starting_facts.is_letter || starting_facts.is_number || starting_facts.is_symbol) {
            match_start += code_unit_length_for_code_point(starting_code_point);
            continue;
        }

        if has_preceding {
            // Preceding group: P followed by (Zs|P)*.
            cursor = host.next_grapheme_boundary(cursor);
            while cursor < code_units {
                let code_point = host.code_point_at(cursor);
                let facts = host.code_point_facts(code_point);
                // For the preceding run: Zs excluding U+3000 IDEOGRAPHIC SPACE.
                let is_preceding_intervening_space = code_point != 0x3000 && facts.is_space_separator;
                if !facts.is_punctuation && !is_preceding_intervening_space {
                    break;
                }
                cursor = host.next_grapheme_boundary(cursor);
            }
        }

        // The letter (L|N|S) must follow the preceding group. If the preceding punctuation consumed the entire text
        // node, accept it as the first-letter.
        if cursor >= code_units {
            return FfiFirstLetterTarget {
                text_node: std::ptr::null_mut(),
                letter_start: match_start,
                letter_end: cursor,
                found: true,
            };
        }
        let letter_facts = host.code_point_facts(host.code_point_at(cursor));
        if !(letter_facts.is_letter || letter_facts.is_number || letter_facts.is_symbol) {
            match_start += code_unit_length_for_code_point(starting_code_point);
            continue;
        }

        let mut letter_end = host.next_grapheme_boundary(cursor);

        // Trailing group: greedy match of (Zs|P-(Ps|Pd))*.
        while letter_end < code_units {
            let code_point = host.code_point_at(letter_end);
            let facts = host.code_point_facts(code_point);
            // For the trailing run: Zs excluding U+3000 IDEOGRAPHIC SPACE and word separators.
            // NB: css-text-4 defines word separators as a non-exhaustive list, but of the seven code
            //     points it names only U+0020 SPACE and U+00A0 NO-BREAK SPACE are in the Zs category;
            //     the rest are in Po and would never reach this check. Fixed-width spaces are explicitly
            //     not word separators per the spec's note, so they remain valid intervening Zs here.
            let is_trailing_intervening_space =
                !matches!(code_point, 0x0020 | 0x00a0 | 0x3000) && facts.is_space_separator;
            // NB: The css-pseudo specification excludes Ps and Pd classes (closing punctuation and dashes) from the
            //     trailing run, whereas CSS 2.1 allowed all classes in both the preceding and trailing runs.
            let is_trailing_punctuation =
                facts.is_punctuation && !facts.is_open_punctuation && !facts.is_dash_punctuation;
            if !is_trailing_intervening_space && !is_trailing_punctuation {
                break;
            }
            letter_end = host.next_grapheme_boundary(letter_end);
        }

        return FfiFirstLetterTarget {
            text_node: std::ptr::null_mut(),
            letter_start: match_start,
            letter_end,
            found: true,
        };
    }
    FfiFirstLetterTarget::not_found()
}

/// Finds the first-letter text pattern within one layout text node.
///
/// # Safety
///
/// `callbacks` must point to a valid callback table whose context remains live for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_find_first_letter_in_text(
    callbacks: *const FfiFirstLetterTextCallbacks,
    preserves_segment_breaks: bool,
) -> FfiFirstLetterTarget {
    abort_on_panic(|| {
        assert!(!callbacks.is_null());
        // SAFETY: The caller guarantees that `callbacks` remains valid for the duration of this call.
        let host = FirstLetterTextHost {
            callbacks: unsafe { &*callbacks },
        };
        find_first_letter_in_text(&host, preserves_segment_breaks)
    })
}

fn is_marker_content(facts: FfiLayoutNodeFacts) -> bool {
    facts.is_list_item_marker_box || facts.is_generated_for_marker
}

// https://drafts.csswg.org/css-pseudo-4/#first-letter-application
fn find_first_letter_in_block(host: &TreeBuilderHost<'_>, block: LayoutNode) -> FfiFirstLetterTarget {
    // NB: This walks a block container's inline descendants looking for the first-letter text. If the block has block
    //     children instead of inline, recurses into each in-flow block child in turn.
    if host.facts(block).children_are_inline {
        let mut result = FfiFirstLetterTarget::not_found();
        let mut is_root = true;
        host.for_each_in_inclusive_subtree(block, |node| {
            if is_root {
                is_root = false;
                return TraversalDecision::Continue;
            }
            let facts = host.facts(node);
            if is_marker_content(facts) || facts.is_out_of_flow {
                return TraversalDecision::SkipChildrenAndContinue;
            }
            if facts.is_text {
                // SAFETY: `node` is a live TextNode.
                result = unsafe { (host.callbacks.find_first_letter_in_text)(host.callbacks.context, node) };
                return if result.found {
                    TraversalDecision::Break
                } else {
                    TraversalDecision::Continue
                };
            }
            if facts.is_fragmented_inline {
                return TraversalDecision::Continue;
            }
            TraversalDecision::Break
        });
        return result;
    }

    // We have no inline content of our own but ::first-letter can still apply to text in an in-flow block descendant,
    // so walk into each in-flow block child in document order until one yields a letter.
    let mut child = host.first_child(block);
    while !child.is_null() {
        let facts = host.facts(child);
        if is_marker_content(facts) || facts.is_out_of_flow {
            child = host.next_sibling(child);
            continue;
        }
        if !facts.is_block_container {
            break;
        }
        // Stop descending if this child block defines its own ::first-letter: the child will style the first letter
        // inside it, so the ancestor's ::first-letter must not also claim the same letter.
        if facts.has_first_letter_style {
            break;
        }
        let target = find_first_letter_in_block(host, child);
        if target.found {
            return target;
        }
        if !facts.is_anonymous {
            break;
        }
        child = host.next_sibling(child);
    }
    FfiFirstLetterTarget::not_found()
}

/// Finds the text range claimed by a block's `::first-letter` pseudo-element.
///
/// # Safety
///
/// `callbacks` must remain valid for the duration of the call and `block` must be a live BlockContainer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_find_first_letter_in_block(
    callbacks: *const FfiTreeBuilderCallbacks,
    block: *mut c_void,
) -> FfiFirstLetterTarget {
    abort_on_panic(|| {
        assert!(!callbacks.is_null());
        assert!(!block.is_null());
        // SAFETY: The caller guarantees that `callbacks` remains valid for the duration of this call.
        let host = TreeBuilderHost {
            callbacks: unsafe { &*callbacks },
        };
        find_first_letter_in_block(&host, block)
    })
}

/// Creates the anonymous layout structure required for button rendering.
///
/// # Safety
///
/// `callbacks` must remain valid for the call and `layout_node` must be a live NodeWithStyle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_wrap_button_contents_if_needed(
    callbacks: *const FfiTreeBuilderCallbacks,
    layout_node: *mut c_void,
    uses_button_layout: bool,
) {
    abort_on_panic(|| {
        assert!(!callbacks.is_null());
        assert!(!layout_node.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let host = TreeBuilderHost {
            callbacks: unsafe { &*callbacks },
        };
        if !uses_button_layout {
            return;
        }

        // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
        // If the element is an input element, or if it is a button element and its computed value for 'display' is not
        // 'inline-grid', 'grid', 'inline-flex', or 'flex', then the element's box has a child anonymous button content
        // box with the following behaviors:
        let facts = host.facts(layout_node);
        if !facts.display_is_grid_inside && !facts.display_is_flex_inside {
            let mut children = Vec::new();
            let mut child = host.first_child(layout_node);
            while !child.is_null() {
                children.push(child);
                child = host.next_sibling(child);
            }

            // SAFETY: `layout_node` remains live and owns the returned content wrapper through its flex wrapper.
            let content_wrapper =
                unsafe { (host.callbacks.create_button_content_wrapper)(host.callbacks.context, layout_node) };
            assert!(!content_wrapper.is_null());
            // SAFETY: The parent and content wrapper remain live, and the callback retains all nodes while moving them.
            unsafe {
                (host.callbacks.set_children_are_inline)(
                    host.callbacks.context,
                    content_wrapper,
                    facts.children_are_inline,
                );
                (host.callbacks.move_nodes_to_parent)(
                    host.callbacks.context,
                    content_wrapper,
                    children.as_ptr(),
                    children.len(),
                );
                (host.callbacks.set_children_are_inline)(host.callbacks.context, layout_node, false);
            }
        }
    });
}

/// Creates the anonymous fieldset content box around non-legend children.
///
/// # Safety
///
/// `callbacks` must remain valid for the call and `layout_node` must be a live layout node.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_wrap_fieldset_contents_if_needed(
    callbacks: *const FfiTreeBuilderCallbacks,
    layout_node: *mut c_void,
) {
    abort_on_panic(|| {
        assert!(!callbacks.is_null());
        assert!(!layout_node.is_null());
        // SAFETY: Guaranteed by the entry point's contract.
        let host = TreeBuilderHost {
            callbacks: unsafe { &*callbacks },
        };

        // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
        // The anonymous fieldset content box is expected to appear after the rendered legend and is expected to contain
        // the content (including the '::before' and '::after' pseudo-elements) of the fieldset element except for the
        // rendered legend, if there is one.
        let facts = host.facts(layout_node);
        if facts.is_field_set_box && facts.has_rendered_legend {
            // SAFETY: `layout_node` is a live FieldSetBox with a rendered legend.
            let legend = unsafe { (host.callbacks.rendered_legend)(host.callbacks.context, layout_node) };
            assert!(!legend.is_null());

            let mut children = Vec::new();
            let mut child = host.first_child(layout_node);
            while !child.is_null() {
                if child != legend {
                    children.push(child);
                }
                child = host.next_sibling(child);
            }

            // SAFETY: The fieldset remains live and owns the returned wrapper.
            let wrapper =
                unsafe { (host.callbacks.create_fieldset_content_wrapper)(host.callbacks.context, layout_node) };
            assert!(!wrapper.is_null());
            // SAFETY: The wrapper remains attached and the callback retains all nodes while moving them.
            unsafe {
                (host.callbacks.move_nodes_to_parent)(
                    host.callbacks.context,
                    wrapper,
                    children.as_ptr(),
                    children.len(),
                );
            }
        }
    });
}

fn is_table_track(display: FfiTableDisplay) -> bool {
    matches!(display, FfiTableDisplay::TableRow | FfiTableDisplay::TableColumn)
}

fn is_table_track_group(display: FfiTableDisplay) -> bool {
    // Unless explicitly mentioned otherwise, mentions of table-row-groups in this spec also encompass the specialized
    // table-header-groups and table-footer-groups.
    matches!(
        display,
        FfiTableDisplay::TableRowGroup
            | FfiTableDisplay::TableHeaderGroup
            | FfiTableDisplay::TableFooterGroup
            | FfiTableDisplay::TableColumnGroup
    )
}

fn display_for_table_fixup(facts: FfiLayoutNodeFacts) -> FfiTableDisplay {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // For the purposes of these rules, out-of-flow elements are represented as inline elements of zero width and
    // height. Their containing blocks are chosen accordingly.
    //
    // AD-HOC: Table-internal boxes can be blockified before fixup. Use the pre-transformation display for authored
    // boxes so an out-of-flow table-header-group is still recognized as a proper table child during fixup.
    if facts.has_replaced_element_table_display_adjustment || facts.is_anonymous {
        facts.current_display
    } else {
        facts.display_before_box_type_transformation
    }
}

fn is_proper_table_child(facts: FfiLayoutNodeFacts) -> bool {
    let display = display_for_table_fixup(facts);
    is_table_track_group(display) || is_table_track(display) || display == FfiTableDisplay::TableCaption
}

fn is_table_non_root_box_with_display(display: FfiTableDisplay) -> bool {
    matches!(
        display,
        FfiTableDisplay::TableRow
            | FfiTableDisplay::TableColumn
            | FfiTableDisplay::TableRowGroup
            | FfiTableDisplay::TableHeaderGroup
            | FfiTableDisplay::TableFooterGroup
            | FfiTableDisplay::TableColumnGroup
            | FfiTableDisplay::TableCell
            | FfiTableDisplay::TableCaption
    )
}

fn is_table_non_root_box(facts: FfiLayoutNodeFacts) -> bool {
    is_table_non_root_box_with_display(facts.current_display)
}

fn is_tabular_container(facts: FfiLayoutNodeFacts) -> bool {
    // https://drafts.csswg.org/css-tables-3/#tabular-container
    matches!(
        facts.current_display,
        FfiTableDisplay::TableRoot
            | FfiTableDisplay::TableRow
            | FfiTableDisplay::TableRowGroup
            | FfiTableDisplay::TableHeaderGroup
            | FfiTableDisplay::TableFooterGroup
    )
}

fn is_ignorable_whitespace(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    let facts = host.facts(node);
    if facts.is_text && facts.text_is_ascii_whitespace {
        return true;
    }

    if facts.is_anonymous && facts.is_block_container && facts.children_are_inline {
        let mut contains_only_whitespace = true;
        host.for_each_in_inclusive_subtree(node, |descendant| {
            let descendant_facts = host.facts(descendant);
            if descendant_facts.is_text {
                if !descendant_facts.text_is_ascii_whitespace {
                    contains_only_whitespace = false;
                    return TraversalDecision::Break;
                }
            } else if descendant_facts.is_out_of_flow || !descendant_facts.is_anonymous {
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
    if !previous_sibling.is_null() && !next_sibling.is_null() {
        return false;
    }
    if !previous_sibling.is_null() && !is_table_non_root_box(host.facts(previous_sibling)) {
        return false;
    }
    if !next_sibling.is_null() && !is_table_non_root_box(host.facts(next_sibling)) {
        return false;
    }
    true
}

fn for_each_sequence_of_consecutive_children_matching(
    host: &TreeBuilderHost<'_>,
    parent: LayoutNode,
    matcher: impl Fn(FfiLayoutNodeFacts) -> bool,
    mut callback: impl FnMut(&[LayoutNode], LayoutNode),
) {
    let mut sequence = Vec::new();
    let mut child = host.first_child(parent);
    while !child.is_null() {
        if matcher(host.facts(child)) || (!sequence.is_empty() && is_ignorable_whitespace(host, child)) {
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
        callback(&sequence, std::ptr::null_mut());
    }
}

fn remove_irrelevant_boxes(host: &TreeBuilderHost<'_>, root: LayoutNode) {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 1. Remove irrelevant boxes:
    // The following boxes are discarded as if they were display:none:
    let mut to_remove = Vec::new();
    host.for_each_in_inclusive_subtree(root, |node| {
        let facts = host.facts(node);

        // 1. Children of a table-column.
        if facts.is_box && facts.current_display == FfiTableDisplay::TableColumn {
            let mut child = host.first_child(node);
            while !child.is_null() {
                to_remove.push(child);
                child = host.next_sibling(child);
            }
        }

        // 2. Children of a table-column-group which are not a table-column.
        if facts.is_box && facts.current_display == FfiTableDisplay::TableColumnGroup {
            let mut child = host.first_child(node);
            while !child.is_null() {
                if host.facts(child).current_display != FfiTableDisplay::TableColumn {
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
        if facts.is_box
            && !parent.is_null()
            && is_tabular_container(host.facts(parent))
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
        let facts = host.facts(parent);
        if !facts.is_box {
            return TraversalDecision::Continue;
        }

        match facts.current_display {
            FfiTableDisplay::TableRoot => {
                // 1. An anonymous table-row box must be generated around each sequence of consecutive children of a
                //    table-root box which are not proper table child boxes.
                for_each_sequence_of_consecutive_children_matching(
                    host,
                    parent,
                    |child| !child.has_style || !is_proper_table_child(child),
                    |sequence, nearest_sibling| {
                        host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableRow);
                    },
                );
            }
            FfiTableDisplay::TableRowGroup | FfiTableDisplay::TableHeaderGroup | FfiTableDisplay::TableFooterGroup => {
                // 2. An anonymous table-row box must be generated around each sequence of consecutive children of a
                //    table-row-group box which are not table-row boxes.
                for_each_sequence_of_consecutive_children_matching(
                    host,
                    parent,
                    |child| !child.has_style || child.current_display != FfiTableDisplay::TableRow,
                    |sequence, nearest_sibling| {
                        host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableRow);
                    },
                );
            }
            FfiTableDisplay::TableRow => {
                // 3. An anonymous table-cell box must be generated around each sequence of consecutive children of a
                //    table-row box which are not table-cell boxes.
                for_each_sequence_of_consecutive_children_matching(
                    host,
                    parent,
                    |child| !child.has_style || child.current_display != FfiTableDisplay::TableCell,
                    |sequence, nearest_sibling| {
                        host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableCell);
                    },
                );
            }
            _ => {}
        }
        TraversalDecision::Continue
    });
}

fn generate_missing_parents(host: &TreeBuilderHost<'_>, root: LayoutNode) -> Vec<LayoutNode> {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 3. Generate missing parents:
    let mut table_roots_to_wrap = Vec::new();
    host.for_each_in_inclusive_subtree(root, |parent| {
        let facts = host.facts(parent);
        if !facts.has_style {
            return TraversalDecision::Continue;
        }

        // 1. An anonymous table-row box must be generated around each sequence of consecutive table-cell boxes whose
        //    parent is not a table-row.
        if facts.current_display != FfiTableDisplay::TableRow {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| child.has_style && child.current_display == FfiTableDisplay::TableCell,
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
        let anonymous_table_kind = if facts.is_inline_outside {
            FfiAnonymousTableBoxKind::InlineTable
        } else {
            FfiAnonymousTableBoxKind::Table
        };

        let is_table_row_group = matches!(
            facts.current_display,
            FfiTableDisplay::TableRowGroup | FfiTableDisplay::TableHeaderGroup | FfiTableDisplay::TableFooterGroup
        );
        // A table-row is misparented if its parent is neither a table-row-group nor a table-root box.
        if !is_table_row_group && facts.current_display != FfiTableDisplay::TableRoot {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| child.has_style && child.current_display == FfiTableDisplay::TableRow,
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, anonymous_table_kind);
                },
            );
        }

        // A table-column box is misparented if its parent is neither a table-column-group box nor a table-root box.
        if facts.current_display != FfiTableDisplay::TableColumnGroup
            && facts.current_display != FfiTableDisplay::TableRoot
        {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| child.has_style && child.current_display == FfiTableDisplay::TableColumn,
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, anonymous_table_kind);
                },
            );
        }

        // A table-row-group, table-column-group, or table-caption box is misparented if its parent is not a table-root
        // box.
        if facts.current_display != FfiTableDisplay::TableRoot {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| {
                    if !child.has_style {
                        return false;
                    }
                    let display = display_for_table_fixup(child);
                    is_table_track_group(display) || display == FfiTableDisplay::TableCaption
                },
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, anonymous_table_kind);
                },
            );
        }

        // 3. An anonymous table-wrapper box must be generated around each table-root.
        if facts.is_box && facts.current_display == FfiTableDisplay::TableRoot {
            if facts.has_been_wrapped_in_table_wrapper {
                let parent = host.parent(parent);
                assert!(!parent.is_null());
                assert!(host.facts(parent).is_table_wrapper);
                return TraversalDecision::Continue;
            }
            table_roots_to_wrap.push(parent);
        }

        TraversalDecision::Continue
    });

    for &table_root in &table_roots_to_wrap {
        let nearest_sibling = host.next_sibling(table_root);
        let parent = host.parent(table_root);
        assert!(!parent.is_null());
        if host.facts(parent).is_table_wrapper {
            // SAFETY: Both nodes are live and `parent` is a TableWrapper.
            unsafe { (host.callbacks.update_existing_table_wrapper)(host.callbacks.context, table_root, parent) };
        } else {
            // SAFETY: `table_root` is attached and `nearest_sibling` is either null or its live next sibling.
            unsafe { (host.callbacks.wrap_table_root)(host.callbacks.context, table_root, nearest_sibling) };
        }
    }
    table_roots_to_wrap
}

struct TableGrid<'a> {
    host: &'a TreeBuilderHost<'a>,
    grid: *mut c_void,
}

impl TableGrid<'_> {
    fn column_count(&self) -> usize {
        // SAFETY: `grid` is live until this wrapper is dropped.
        unsafe { (self.host.callbacks.table_grid_column_count)(self.host.callbacks.context, self.grid) }
    }

    fn is_occupied(&self, column_index: usize, row_index: usize) -> bool {
        // SAFETY: `grid` is live until this wrapper is dropped.
        unsafe {
            (self.host.callbacks.table_grid_is_occupied)(
                self.host.callbacks.context,
                self.grid,
                column_index,
                row_index,
            )
        }
    }
}

impl Drop for TableGrid<'_> {
    fn drop(&mut self) {
        // SAFETY: This wrapper owns the grid returned by `create_table_grid` and drops it exactly once.
        unsafe { (self.host.callbacks.destroy_table_grid)(self.host.callbacks.context, self.grid) };
    }
}

fn fixup_row(host: &TreeBuilderHost<'_>, row: LayoutNode, table_grid: &TableGrid<'_>, row_index: usize) {
    for column_index in 0..table_grid.column_count() {
        if table_grid.is_occupied(column_index, row_index) {
            continue;
        }
        // SAFETY: `row` is a live table-row box.
        unsafe { (host.callbacks.append_missing_table_cell)(host.callbacks.context, row) };
    }
}

fn missing_cells_fixup(host: &TreeBuilderHost<'_>, table_roots: &[LayoutNode]) {
    // https://drafts.csswg.org/css-tables-3/#missing-cells-fixup
    // Once the amount of columns in a table is known, any table-row box must be modified such that it owns enough
    // cells to fill all the columns of the table, when taking spans into account. New table-cell anonymous boxes must
    // be appended to its rows content until this condition is met.
    for &table_root in table_roots {
        // SAFETY: `table_root` is a live table-root box and the callback returns a newly owned grid.
        let grid = unsafe { (host.callbacks.create_table_grid)(host.callbacks.context, table_root) };
        assert!(!grid.is_null());
        let grid = TableGrid { host, grid };
        let mut row_index = 0;

        let mut child = host.first_child(table_root);
        while !child.is_null() {
            let child_facts = host.facts(child);
            if child_facts.is_box
                && matches!(
                    child_facts.current_display,
                    FfiTableDisplay::TableRowGroup
                        | FfiTableDisplay::TableHeaderGroup
                        | FfiTableDisplay::TableFooterGroup
                )
            {
                let mut row = host.first_child(child);
                while !row.is_null() {
                    let row_facts = host.facts(row);
                    if row_facts.is_box && row_facts.current_display == FfiTableDisplay::TableRow {
                        fixup_row(host, row, &grid, row_index);
                        row_index += 1;
                    }
                    row = host.next_sibling(row);
                }
            }
            child = host.next_sibling(child);
        }

        let mut child = host.first_child(table_root);
        while !child.is_null() {
            let child_facts = host.facts(child);
            if child_facts.is_box && child_facts.current_display == FfiTableDisplay::TableRow {
                fixup_row(host, child, &grid, row_index);
                row_index += 1;
            }
            child = host.next_sibling(child);
        }
    }
}

/// Runs the CSS table-model fixup over an already constructed layout subtree.
///
/// # Safety
///
/// `callbacks` must point to a valid callback table for the duration of the call. `root` must be a live
/// NodeWithStyle, and every callback-returned node must remain live for as long as it stays attached to that root.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_fixup_tables(callbacks: *const FfiTreeBuilderCallbacks, root: *mut c_void) {
    abort_on_panic(|| {
        assert!(!callbacks.is_null());
        assert!(!root.is_null());
        // SAFETY: Guaranteed by the entry point's contract and checked for null above.
        let host = TreeBuilderHost {
            callbacks: unsafe { &*callbacks },
        };
        remove_irrelevant_boxes(&host, root);
        generate_missing_child_wrappers(&host, root);
        let table_roots = generate_missing_parents(&host, root);
        missing_cells_fixup(&host, &table_roots);
    });
}

#[cfg(test)]
mod tests {
    use super::{
        FfiComputedContentType, FfiElementLayoutFacts, FfiElementLayoutKind, FfiFirstLetterCodePointFacts,
        FfiFirstLetterTextCallbacks, FfiPrincipalBoxPlacement, FfiPrincipalBoxPlacementFacts,
        FfiPrincipalNodeEntryFacts, FfiPseudoElement, FfiPseudoElementDecision, FfiPseudoElementFacts,
        FfiReplacedElementDisplayAdjustment, FirstLetterTextHost, PrincipalBoxGenerationDecision, SvgEntryDecision,
        TopLayerEntryDecision, adjusted_table_display_for_replaced_element, element_layout_kind,
        find_first_letter_in_text, principal_box_generation_decision, principal_box_placement_decision,
        principal_node_entry_decision, pseudo_element_decision, rust_display_contents_text_needs_style_wrapper,
    };
    use std::ffi::c_void;

    unsafe extern "C" fn text_length(context: *mut c_void) -> usize {
        // SAFETY: Test callers pass a valid `Vec<u16>` as the callback context.
        unsafe { (&*context.cast::<Vec<u16>>()).len() }
    }

    unsafe extern "C" fn code_point_at(context: *mut c_void, index: usize) -> u32 {
        // SAFETY: Test callers pass a valid `Vec<u16>` and an in-bounds index.
        unsafe { (&*context.cast::<Vec<u16>>())[index] as u32 }
    }

    unsafe extern "C" fn next_grapheme_boundary(_: *mut c_void, index: usize) -> usize {
        index + 1
    }

    unsafe extern "C" fn code_point_facts(_: *mut c_void, code_point: u32) -> FfiFirstLetterCodePointFacts {
        FfiFirstLetterCodePointFacts {
            is_space_separator: code_point == b' ' as u32,
            is_punctuation: matches!(code_point, 0x21 | 0x22 | 0x27..=0x2f | 0x3a | 0x3b | 0x3f | 0x40),
            is_letter: matches!(code_point, 0x41..=0x5a | 0x61..=0x7a),
            is_number: matches!(code_point, 0x30..=0x39),
            is_symbol: matches!(code_point, 0x24 | 0x2b | 0x3c..=0x3e | 0x5e | 0x60 | 0x7c | 0x7e),
            is_open_punctuation: matches!(code_point, 0x28 | 0x5b | 0x7b),
            is_dash_punctuation: code_point == 0x2d,
        }
    }

    fn first_letter_target(text: &str, preserves_segment_breaks: bool) -> super::FfiFirstLetterTarget {
        let mut text = text.encode_utf16().collect::<Vec<_>>();
        let callbacks = FfiFirstLetterTextCallbacks {
            context: (&raw mut text).cast(),
            code_unit_length: text_length,
            code_point_at,
            next_grapheme_boundary,
            code_point_facts,
        };
        find_first_letter_in_text(&FirstLetterTextHost { callbacks: &callbacks }, preserves_segment_breaks)
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
    fn tree_builder_state_tracks_ancestors_and_quotes() {
        let state = super::rust_tree_builder_state_create();
        let mut parent = 0_u8;
        let parent_pointer = (&raw mut parent).cast::<c_void>();
        super::rust_tree_builder_push_parent(state, parent_pointer);
        assert_eq!(super::rust_tree_builder_ancestor_count(state), 1);
        assert_eq!(super::rust_tree_builder_current_parent(state), parent_pointer);
        assert_eq!(super::rust_tree_builder_ancestor_at(state, 0), parent_pointer);

        super::rust_tree_builder_set_quote_nesting_level(state, 3);
        assert_eq!(super::rust_tree_builder_quote_nesting_level(state), 3);

        super::rust_tree_builder_pop_parent(state);
        assert_eq!(super::rust_tree_builder_ancestor_count(state), 0);
        // SAFETY: `state` was returned by the matching create function and has not been destroyed yet.
        unsafe { super::rust_tree_builder_state_destroy(state) };
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
            FfiPseudoElementDecision::NormalMarker
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
            document_needs_full_layout_tree_update: false,
            is_document: false,
            has_layout_node: true,
            is_element: true,
            is_text: false,
            rendered_in_top_layer: false,
            context_layout_top_layer: false,
            layout_node_is_attached: true,
            is_svg_container: false,
            requires_svg_container: false,
            context_has_svg_root: false,
        };
        let decision = principal_node_entry_decision(facts);
        assert!(!decision.should_create_layout_node);
        assert_eq!(decision.top_layer, TopLayerEntryDecision::Continue);
        assert_eq!(decision.svg, SvgEntryDecision::Continue);

        facts.rendered_in_top_layer = true;
        facts.layout_node_is_attached = false;
        let decision = principal_node_entry_decision(facts);
        assert_eq!(decision.top_layer, TopLayerEntryDecision::SkipAndRequestZoneRebuild);

        facts.rendered_in_top_layer = false;
        facts.requires_svg_container = true;
        let decision = principal_node_entry_decision(facts);
        assert_eq!(decision.svg, SvgEntryDecision::Skip);

        facts.must_create_subtree = true;
        facts.is_svg_container = true;
        let decision = principal_node_entry_decision(facts);
        assert!(decision.should_create_layout_node);
        assert_eq!(decision.svg, SvgEntryDecision::EnterSvgRoot);
    }

    #[test]
    fn specialized_element_layout_kinds() {
        let mut facts = FfiElementLayoutFacts {
            has_content_replacement: false,
            context_layout_svg_mask_or_clip_path: false,
            context_layout_svg_pattern: false,
            is_svg_mask_element: false,
            is_svg_clip_path_element: false,
            is_svg_pattern_element: false,
        };
        assert_eq!(element_layout_kind(facts), FfiElementLayoutKind::Normal);

        facts.has_content_replacement = true;
        assert_eq!(element_layout_kind(facts), FfiElementLayoutKind::ContentReplacement);
        facts.has_content_replacement = false;
        facts.context_layout_svg_mask_or_clip_path = true;
        facts.is_svg_mask_element = true;
        assert_eq!(element_layout_kind(facts), FfiElementLayoutKind::SvgMask);

        facts.context_layout_svg_mask_or_clip_path = false;
        facts.is_svg_mask_element = false;
        facts.context_layout_svg_pattern = true;
        facts.is_svg_pattern_element = true;
        assert_eq!(element_layout_kind(facts), FfiElementLayoutKind::SvgPattern);
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

        let mut facts = FfiPrincipalBoxPlacementFacts {
            must_create_subtree: false,
            should_create_layout_node: true,
            has_old_layout_node: true,
            old_layout_node_is_attached: true,
            old_and_new_layout_nodes_are_same: false,
            has_current_rebuild_root: false,
            is_document: false,
            is_element: true,
            rendered_in_top_layer: true,
            context_layout_top_layer: true,
            layout_node_is_svg_box: false,
        };
        let decision = principal_box_placement_decision(facts);
        assert_eq!(decision.placement, FfiPrincipalBoxPlacement::ReplaceExisting);
        assert!(decision.start_rebuild_root);
        assert!(decision.create_backdrop);
        assert!(decision.clear_layout_top_layer_for_descendants);

        facts.has_old_layout_node = false;
        facts.old_layout_node_is_attached = false;
        facts.layout_node_is_svg_box = true;
        let decision = principal_box_placement_decision(facts);
        assert_eq!(decision.placement, FfiPrincipalBoxPlacement::AppendSvg);
        assert!(decision.mark_update_escaped_rebuild_roots);
    }

    #[test]
    fn display_contents_text_style_wrapper_decisions() {
        assert!(!rust_display_contents_text_needs_style_wrapper(
            false, true, false, false
        ));
        assert!(rust_display_contents_text_needs_style_wrapper(true, true, false, true));
        assert!(!rust_display_contents_text_needs_style_wrapper(true, true, true, true));
        assert!(rust_display_contents_text_needs_style_wrapper(true, true, true, false));
    }
}
