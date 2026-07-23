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
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_update_layout_tree_for_dom_children(
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
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_update_layout_tree_for_shadow_root_children(
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
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_update_layout_tree_for_assigned_slottables(
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
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_update_layout_tree_for_svg_switch_children(
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
    pub pseudo_element: FfiPseudoElement,
    pub content_type: FfiComputedContentType,
    pub display_is_none: bool,
    pub display_is_contents: bool,
    pub display_is_list_item: bool,
    pub has_content_replacement: bool,
    pub originating_layout_node_is_list_item: bool,
    pub normal_marker_has_content: bool,
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_pseudo_element_decision(facts: FfiPseudoElementFacts) -> FfiPseudoElementDecision {
    abort_on_panic(|| {
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

#[unsafe(no_mangle)]
pub extern "C" fn rust_adjusted_table_display_for_replaced_element(
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
        FfiComputedContentType, FfiFirstLetterCodePointFacts, FfiFirstLetterTextCallbacks, FfiPseudoElement,
        FfiPseudoElementDecision, FfiPseudoElementFacts, FfiReplacedElementDisplayAdjustment, FirstLetterTextHost,
        find_first_letter_in_text, rust_adjusted_table_display_for_replaced_element, rust_pseudo_element_decision,
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
            rust_adjusted_table_display_for_replaced_element(true, true, false, false),
            FfiReplacedElementDisplayAdjustment::Block
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(true, false, false, false),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(false, false, true, false),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(false, false, false, true),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(false, false, false, false),
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
            rust_pseudo_element_decision(FfiPseudoElementFacts {
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
}
