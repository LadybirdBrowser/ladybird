/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

use crate::css::css_enums::{content_visibility, white_space_collapse};
use crate::css::style::RecordDemand;
use crate::css::style::RetainedState;
use crate::css::style::bridge::{
    ElementBoxKind, FfiDemandedPseudoElement, FfiPseudoElementRecordDemand, answer_record_demand,
    element_adjustment_fact,
};
use crate::css::style::index::StyleAtomID;
use crate::css::style::layout_style::{AnonymousStyleKind, AnonymousStyleOverrides, DerivedStyleRecord, LayoutStyle};
use crate::css::style::tree::StyleNodeID;
use crate::layout::layout_node_arena::{LayoutNodeArena, OwedImageResources, StaleWalkFacts};
use crate::layout::node_data::{
    GENERATED_FOR_AFTER, GENERATED_FOR_BACKDROP, GENERATED_FOR_BEFORE, GENERATED_FOR_FIRST_LETTER,
    GENERATED_FOR_MARKER, NodeData, NodeFlag, NodeKind, NodeSlotId, pseudo_kind_of,
};
use crate::layout::quote_depth::QuoteDepthCursor;
use crate::layout::text_chunker::{GraphemeSegmenter, code_point_at, code_unit_length_for_code_point};
use crate::layout::tree_mutation::{HostCalls, OwedHostWork, UnplacedLayoutNode};
use crate::layout::tree_plan::planner::StaleSubtreeScope;
use crate::layout::tree_plan::table_fixup::{AnonymousTableBoxKind, display_for_table_fixup, is_tabular_container};
use crate::layout::tree_update_marks::layout_tree_update_reuse_reason;
use crate::layout::{ComputedValuesView, FfiDisplay};
use crate::painting::paint_read::PaintRead;

mod driver;
mod list_unit;
mod planned_build;

type LayoutNode = NodeSlotId;

#[derive(Default)]
pub(crate) struct TreeBuilderState {
    quote_depth_cursor: Option<QuoteDepthCursor>,
    /// The boxes whose subtrees the build built again, for the partial relayout that follows it, and whether a change
    /// reached beyond what such a relayout confines.
    rebuilt_subtree_roots: Vec<LayoutNode>,
    layout_tree_update_escaped_rebuild_roots: bool,
    /// The boxes of the child lists that gained boxes, each with the boxes that went into it.
    child_lists_that_gained_boxes: Vec<(LayoutNode, Vec<LayoutNode>)>,
    /// The boxes whose `::first-letter` is split out again once the build is over, see
    /// [`list_unit::split_first_letters_again`].
    first_letter_owners_to_split_again: Vec<StyleNodeID>,
    /// Whether the build changed the quote depth of boxes it kept, which only a build of every box finds again.
    asks_for_every_box_again: bool,
    /// Whether the build reached an element no style update settled, which it built no box for.
    reached_unstyled_element: bool,
    /// What the build found out that the document has to be told, in the order it found it out.
    /// Delivered when the walk ends: nothing inside the build reads any of it back.
    reports: Vec<crate::layout::commit::FfiCommitMessage>,
    /// Every style record a box is built from, held until the build ends, so that a restyle later
    /// in the same build cannot take it away from a box that names it. Letting go of one the build
    /// has stopped looking at buys nothing before it ends.
    pinned_style_records: Vec<u64>,
    /// The document's style, which the build is handed before it starts when it may build the
    /// viewport, held until the viewport's row takes it or the build ends without one.
    pub(super) document_style: Option<DerivedStyleRecord>,
}

impl TreeBuilderState {
    /// Lets go of every record the build held, once the build is over.
    pub(super) fn release_pinned_style_records(&mut self, arena: &mut LayoutNodeArena) {
        let unused_document_style = self.document_style.take().map(|document_style| document_style.record);
        if self.pinned_style_records.is_empty() && unused_document_style.is_none() {
            return;
        }
        arena.with_style_engine(|engine| {
            for record in self.pinned_style_records.drain(..).chain(unused_document_style) {
                engine.unpin_layout_style_record(record);
            }
        });
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ElementLayoutKind {
    ContentReplacement,
    SvgMask,
    SvgClipPath,
    SvgPattern,
    Normal,
}

/// The display the box of `kind`, with construction flags `flags`, uses for `display`, the computed display of its
/// element or pseudo-element.
pub(crate) fn used_display_of_box(kind: NodeKind, flags: u32, display: FfiDisplay) -> FfiDisplay {
    use crate::css::css_enums::{display_inside, display_outside};
    let flow = |outside| FfiDisplay::outside_and_inside(outside, display_inside::FLOW, false);
    // https://drafts.csswg.org/css-content-3/#content-property
    // NB: A `display: contents` pseudo-element is an inline box holding its contents.
    if display.is_contents() {
        return flow(display_outside::INLINE);
    }
    // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
    // If the computed outer display type is inline, the fieldset is expected to behave as inline-block.
    // Otherwise, it is expected to behave as flow-root. This does not change the computed value.
    if kind == NodeKind::FieldSetBox && display.is_flow_inside() {
        return FfiDisplay::outside_and_inside(display.outside, display_inside::FLOW_ROOT, false);
    }
    if flags & NodeFlag::IsReplacedElement as u32 == 0 {
        return display;
    }
    // https://drafts.csswg.org/css-display-3/#outer-role
    // Note: Outer display types do affect replaced elements.
    if display.is_table_inside() {
        return flow(if display.is_inline_outside() {
            display_outside::INLINE
        } else {
            display_outside::BLOCK
        });
    }
    // https://drafts.csswg.org/css-display-3/#layout-specific-display
    // When the 'display' property of a replaced element computes to one of the layout-internal values, it is
    // handled as having a used value of 'display: inline'.
    if display.is_internal_table() || display.is_table_caption() {
        return flow(display_outside::INLINE);
    }
    display
}

/// The kind of box a computed display asks for, or none for a display that generates no box.
pub(crate) fn node_kind_for_display(display: FfiDisplay) -> Option<NodeKind> {
    if display.is_none() || display.is_contents() {
        return None;
    }
    if display.is_table_inside()
        || display.is_table_row_group()
        || display.is_table_header_group()
        || display.is_table_footer_group()
        || display.is_table_row()
    {
        return Some(NodeKind::Box);
    }
    if display.is_list_item() {
        return Some(NodeKind::ListItemBox);
    }
    if display.is_table_cell() {
        return Some(NodeKind::BlockContainer);
    }
    if display.is_table_column() || display.is_table_column_group() || display.is_table_caption() {
        // FIXME: This is just an incorrect placeholder until we improve table layout support.
        return Some(NodeKind::BlockContainer);
    }
    if display.is_math_inside() {
        // https://w3c.github.io/mathml-core/#new-display-math-value
        // MathML elements with a computed display value equal to block math or inline math control box generation
        // and layout according to their tag name, as described in the relevant sections.
        // FIXME: Figure out what kind of node we should make for them. For now, we'll stick with a generic Box.
        return Some(NodeKind::BlockContainer);
    }
    if display.is_inline_outside() {
        if display.is_flow_root_inside() {
            return Some(NodeKind::BlockContainer);
        }
        if display.is_flex_inside() || display.is_grid_inside() {
            return Some(NodeKind::Box);
        }
        return Some(NodeKind::InlineNode);
    }
    if display.is_flex_inside() || display.is_grid_inside() {
        return Some(NodeKind::Box);
    }
    // FIXME: We don't actually support `display: block ruby`, this treats it as a block.
    if display.is_flow_inside() || display.is_flow_root_inside() {
        return Some(NodeKind::BlockContainer);
    }
    Some(NodeKind::InlineNode)
}

/// The kind of principal box an element asks for. The element's own type and state decided its
/// box kind at style time; its computed display decides the box for the kinds that leave the
/// choice to it, and `appearance: none` suppresses an input's native widget.
/// https://drafts.csswg.org/css-ui/#appearance-switching
pub(crate) fn node_kind_for_element_box_kind(
    box_kind: ElementBoxKind,
    display: FfiDisplay,
    appearance: u8,
) -> Option<NodeKind> {
    if appearance == crate::css::css_enums::appearance::NONE && box_kind.is_suppressed_by_appearance_none() {
        return node_kind_for_display(display);
    }
    Some(match box_kind {
        ElementBoxKind::NoBox => return None,
        ElementBoxKind::FromDisplay => return node_kind_for_display(display),
        ElementBoxKind::Break => NodeKind::BreakNode,
        ElementBoxKind::FieldSet => NodeKind::FieldSetBox,
        ElementBoxKind::Legend => NodeKind::LegendBox,
        ElementBoxKind::Audio => NodeKind::AudioBox,
        ElementBoxKind::Video => NodeKind::VideoBox,
        ElementBoxKind::Canvas => NodeKind::CanvasBox,
        ElementBoxKind::NavigableContainerViewport | ElementBoxKind::Frame => NodeKind::NavigableContainerViewport,
        ElementBoxKind::FrameSet => NodeKind::FrameSetBox,
        ElementBoxKind::TextArea => NodeKind::TextAreaBox,
        ElementBoxKind::Image => NodeKind::ImageBox,
        ElementBoxKind::SvgGraphics => NodeKind::SVGGraphicsBox,
        ElementBoxKind::SvgSvg => NodeKind::SVGSVGBox,
        ElementBoxKind::SvgText => NodeKind::SVGTextBox,
        ElementBoxKind::SvgTextPath => NodeKind::SVGTextPathBox,
        ElementBoxKind::SvgForeignObject => NodeKind::SVGForeignObjectBox,
        ElementBoxKind::SvgImage => NodeKind::SVGImageBox,
        ElementBoxKind::SvgGeometry => NodeKind::SVGGeometryBox,
        ElementBoxKind::InputButton => NodeKind::BlockContainer,
        ElementBoxKind::InputCheckBox => NodeKind::CheckBox,
        ElementBoxKind::InputRadioButton => NodeKind::RadioButton,
        ElementBoxKind::InputRange => NodeKind::RangeInputBox,
        ElementBoxKind::InputText => NodeKind::TextInputBox,
    })
}

pub(crate) fn element_layout_kind(
    has_content_replacement: bool,
    element_type_facts: u32,
    layout_svg_mask_or_clip_path: bool,
    layout_svg_pattern: bool,
) -> ElementLayoutKind {
    let has = |fact: u32| element_type_facts & fact != 0;
    if has_content_replacement {
        ElementLayoutKind::ContentReplacement
    } else if layout_svg_mask_or_clip_path {
        if has(element_adjustment_fact::IS_SVG_MASK_ELEMENT) {
            ElementLayoutKind::SvgMask
        } else {
            assert!(has(element_adjustment_fact::IS_SVG_CLIP_PATH_ELEMENT));
            ElementLayoutKind::SvgClipPath
        }
    } else if layout_svg_pattern {
        assert!(has(element_adjustment_fact::IS_SVG_PATTERN_ELEMENT));
        ElementLayoutKind::SvgPattern
    } else {
        ElementLayoutKind::Normal
    }
}

pub(crate) fn display_contents_text_needs_style_wrapper(
    has_style_parent: bool,
    parent_display_is_contents: bool,
    text_is_ascii_whitespace: bool,
    parent_collapses_whitespace: bool,
) -> bool {
    has_style_parent && parent_display_is_contents && (!text_is_ascii_whitespace || !parent_collapses_whitespace)
}

/// Whether an SVG resource box survives the clearing of a DOM subtree.
///
/// SVGPatternBox, SVGMaskBox and SVGClipBox are created on behalf of a referencing element and
/// attached to that element's layout subtree, so they survive cleanup of their DOM ancestor unless
/// their layout attachment is inside the subtree being cleared too. The ancestors are arena rows,
/// and the style mirror answers whether each one's node lies in the cleared subtree. An anonymous
/// row stands for no node; the viewport stands for the document, which only a cleared document
/// contains. A clear that is not bounded to a root keeps every resource box.
fn svg_resource_box_survives(
    arena: &LayoutNodeArena,
    layout_node: NodeSlotId,
    cleared_subtree_root: Option<StyleNodeID>,
) -> bool {
    let Some(cleared_subtree_root) = cleared_subtree_root else {
        return true;
    };
    arena.with_style_store(|engine| {
        // The document holds every node a row stands for.
        let root_is_document = engine.is_relation_only(cleared_subtree_root);
        let mut ancestor = arena.data(layout_node).parent.get();
        while !ancestor.is_invalid() {
            let data = arena.data(ancestor);
            if data.flags.get() & NodeFlag::Anonymous as u32 == 0 {
                let inside = if data.kind.get() == NodeKind::Viewport {
                    root_is_document
                } else {
                    arena.node_style_node(ancestor).is_some_and(|style_node| {
                        root_is_document || engine.is_in_shadow_including_subtree_of(style_node, cleared_subtree_root)
                    })
                };
                if inside {
                    return false;
                }
            }
            ancestor = data.parent.get();
        }
        true
    })
}

/// Finds the box to detach for a top-layer element: the element's own box, or the outermost
/// anonymous wrapper around it that is a direct viewport child. Leaving an empty anonymous
/// table-fixup wrapper as a viewport child would violate layout invariants.
fn topmost_layout_node_of_top_layer_placement(arena: &LayoutNodeArena, layout_node: NodeSlotId) -> NodeSlotId {
    let mut direct_viewport_child_candidate = layout_node;
    loop {
        let parent = arena.data(direct_viewport_child_candidate).parent.get();
        if parent.is_invalid() {
            return NodeSlotId::INVALID;
        }
        let parent_data = arena.data(parent);
        if !node_facts::has_flag(parent_data, NodeFlag::Anonymous) {
            return if parent_data.kind.get() == NodeKind::Viewport {
                direct_viewport_child_candidate
            } else {
                NodeSlotId::INVALID
            };
        }
        direct_viewport_child_candidate = parent;
    }
}

/// Whether an empty text produces a line box fragment: a child of an editing host, or text in a text control, which
/// the caret anchors in and the control takes its baseline from.
pub(crate) fn text_produces_line_box_fragment_when_empty(
    engine: &RetainedState,
    text: StyleNodeID,
    is_in_user_agent_shadow_tree: bool,
) -> bool {
    use crate::css::style::bridge::element_construction_fact::{IS_EDITING_HOST, IS_HTML_INPUT_ELEMENT};
    let parent = engine.text_parent(text);
    let parent_is_editing_host = parent
        .filter(|parent| parent.element_index().is_some())
        .is_some_and(|parent| engine.element_construction_facts(parent) & IS_EDITING_HOST != 0);
    // NB: Tree relations are keyed by element identity, so the walk to the host starts at the text's parent.
    let is_in_text_control = is_in_user_agent_shadow_tree
        && parent
            .and_then(|parent| engine.shadow_host_of(parent))
            .is_some_and(|host| {
                engine.element_construction_facts(host) & IS_HTML_INPUT_ELEMENT != 0
                    || engine.element_box_kind(host) == ElementBoxKind::TextArea
            });
    parent_is_editing_host || is_in_text_control
}

impl TreeBuilderHost<'_> {
    /// The first node in the DOM child sequence the style mirror holds for `parent`.
    fn first_dom_child(&self, parent: StyleNodeID) -> Option<StyleNodeID> {
        self.arena().first_dom_child(parent)
    }

    /// The element facts the style mirror holds for a node the walk reached.
    fn element_type_facts(&self, style_node: Option<StyleNodeID>) -> u32 {
        self.arena().element_adjustment_facts(style_node)
    }

    /// Whether the style mirror holds the element in the top layer.
    fn rendered_in_top_layer(&self, style_node: Option<StyleNodeID>) -> bool {
        self.element_type_facts(style_node) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0
    }
}

/// Whether a flat-tree ancestor of the element keeps it out of the rendered tree.
///
/// Only an element ever hides a subtree, and the mirror steps from a node straight to the element
/// above it, so every ancestor the walk reaches has a published record to ask. No record at all
/// means the style update pass skipped a display:none subtree.
fn has_unrendered_flat_tree_ancestor(host: &TreeBuilderHost<'_>, style_node: Option<StyleNodeID>) -> bool {
    let arena = host.arena();
    let mut ancestor = style_node.and_then(|style_node| arena.flat_tree_parent(style_node));
    while let Some(current) = ancestor {
        if !arena
            .published_box_facts(Some(current))
            .is_some_and(|facts| !facts.display.is_none())
        {
            return true;
        }
        ancestor = arena.flat_tree_parent(current);
    }
    false
}

/// A document's layout tree build, which the host sends its render state: builds or updates the
/// tree from the document's style node, and answers what the build owes the host.
pub(crate) struct TreeBuildJob {
    document_style_node: StyleNodeID,
    /// The document's style, which the host makes rather than publishes, so a build that may build
    /// the viewport is handed it before it starts.
    document_style_record: Option<u64>,
}

/// What a tree build owes the host once it is over, besides the host calls its walk queued, in the
/// order the host pays it.
pub(crate) struct TreeBuildAnswer {
    pub(crate) outcome: FfiLayoutTreeBuildOutcome,
    /// What the build found out that the document has to be told, in the order it found it out.
    pub(crate) reports: Vec<crate::layout::commit::FfiCommitMessage>,
    /// The scroll containers the build gave a style.
    pub(crate) built_scroll_containers: Vec<super::formatting_context::FfiBuiltScrollContainer>,
    /// Whether the build built the whole tree again, rather than only the subtrees it was asked to.
    pub(crate) rebuilt_whole_tree: bool,
}

impl TreeBuildAnswer {
    /// Whether the build shows the value of a `list-item` counter anywhere.
    pub(crate) fn shows_list_item_counter_value(&self) -> bool {
        self.reports
            .iter()
            .any(|report| report.kind == crate::layout::commit::FfiCommitMessageKind::ListItemCounterValueRendered)
    }
}

impl TreeBuildJob {
    pub(crate) fn new(document_style_node: StyleNodeID, document_style_record: Option<u64>) -> Self {
        Self {
            document_style_node,
            document_style_record,
        }
    }

    /// Whether the build may build the viewport, whose style is the document's, which it was not handed.
    pub(crate) fn lacks_document_style(&self, arena: &LayoutNodeArena) -> bool {
        self.document_style_record.is_none() && arena.tree_build_may_create_viewport(self.document_style_node)
    }

    /// Runs the build over the arena of `state`. The host calls the walk owes go into `work`, as the
    /// walk, holding no main thread token, can only queue them; what the host hears of the boxes
    /// nodes gain and lose must be queued already.
    pub(crate) fn run(self, state: &mut ArenaHandle, work: &OwedHostWork) -> TreeBuildAnswer {
        let host = &mut TreeBuilderHost {
            arena: state.arena_mut(),
            work,
        };
        let document_identity = self.document_style_node;
        host.arena_mut().set_document_style_node(document_identity);
        let mut state = TreeBuilderState::default();
        let traces_build = host.arena().layout_trace.records_tree_builds();
        if traces_build {
            host.arena_mut().begin_noting_tree_build_allocations();
        }
        // The viewport's style is the document's, which the host makes rather than publishes, so a
        // build that may build the viewport is handed it before it starts.
        if let Some(record) = self.document_style_record {
            state.document_style = Some(
                host.arena_mut()
                    .with_style_engine(|engine| DerivedStyleRecord::pin(engine, record)),
            );
        }
        // NB: The first build of a document makes its viewport, as does a build that is asked for every box again,
        //     whose viewport replaces the one the document had.
        let builds_viewport = host.arena().layout_root().is_invalid() || host.arena().needs_full_layout_tree_update();
        if builds_viewport {
            driver::build_document(host, &mut state, document_identity);
        } else {
            state.quote_depth_cursor = Some(QuoteDepthCursor::new(host.arena(), document_identity));
            driver::update_marked_child_lists(host, &mut state, document_identity);
        }

        for owner in std::mem::take(&mut state.first_letter_owners_to_split_again) {
            list_unit::split_first_letter_again(host, owner);
        }

        let document_layout_node = host.arena().layout_root();
        let rebuilt_whole_tree = builds_viewport;
        let rebuilt_subtrees_were_updated_individually =
            !rebuilt_whole_tree && !state.layout_tree_update_escaped_rebuild_roots;
        {
            // https://drafts.csswg.org/css-scrollbars/#scrollbar-width
            // UAs must apply the scrollbar-width value set on the root element to the viewport.
            // The document element is the document's only DOM child the style mirror holds: a doctype, a
            // comment and a processing instruction hold no place in its child sequence, and a document
            // can have no text child.
            let root_layout_node = host
                .first_dom_child(document_identity)
                .map_or(NodeSlotId::INVALID, |document_element| {
                    host.arena().bound_row(document_element)
                });
            if !root_layout_node.is_invalid() {
                let scrollbar_width = host
                    .style(root_layout_node)
                    .expect("the document element's box publishes its style during the build")
                    .misc_interaction()
                    .scrollbar_width;
                let host_calls = host.host_calls();
                host.arena_mut()
                    .update_layout_style(host_calls, document_layout_node, |style, _| {
                        style.set_scrollbar_width(scrollbar_width);
                    });
            }
        }

        // NB: A box that moves the quote depth of what follows it asks for every box again.
        if state.asks_for_every_box_again {
            host.arena_mut().set_needs_full_layout_tree_update(true);
        }

        let built_scroll_containers = host.arena_mut().take_built_scroll_containers();
        let arena = host.arena_mut();
        if !rebuilt_subtrees_were_updated_individually {
            // NB: A tree that was not rebuilt subtree by subtree is beyond what a partial relayout plan covers.
            arena.record_partial_relayout_escape();
        }
        arena.invalidate_child_lists_that_gained_boxes(
            &state.child_lists_that_gained_boxes,
            rebuilt_subtrees_were_updated_individually,
        );

        // Table fixup can free a rebuilt root after it was recorded, such as whitespace at the edge of a
        // row group, so only the roots that are still live wait for the partial relayout plan.
        let live_rebuilt_subtree_roots: Vec<NodeSlotId> = state
            .rebuilt_subtree_roots
            .iter()
            .copied()
            .filter(|root| arena.slot_is_live(*root))
            .collect();
        if traces_build {
            let created = arena.take_live_tree_build_allocations();
            arena.layout_trace.note_tree_build(
                rebuilt_whole_tree,
                state.layout_tree_update_escaped_rebuild_roots,
                &live_rebuilt_subtree_roots,
                &created,
            );
        }
        arena.set_pending_rebuilt_subtree_roots(
            live_rebuilt_subtree_roots,
            state.layout_tree_update_escaped_rebuild_roots,
        );
        let viewport = arena.layout_root();
        assert!(!viewport.is_invalid(), "a layout tree build places the viewport");
        state.release_pinned_style_records(arena);
        let outcome = FfiLayoutTreeBuildOutcome {
            viewport,
            needs_another_build_pass: state.asks_for_every_box_again || state.reached_unstyled_element,
        };
        TreeBuildAnswer {
            outcome,
            reports: state.reports,
            built_scroll_containers,
            rebuilt_whole_tree,
        }
    }
}

/// The shadow-including walk that clears stale layout boxes navigates the arena's style mirror by
/// identity.
impl TreeBuilderHost<'_> {
    /// Clears the stale layout box of `node`, and the boxes of its pseudo-elements, answering
    /// whether its subtree survives with it, which only an SVG resource box's does.
    /// `cleared_subtree_root` is the root of the subtree being cleared, or none when the clear is
    /// not bounded to one.
    fn clear_stale_layout_node(&mut self, node: StyleNodeID, cleared_subtree_root: Option<StyleNodeID>) -> bool {
        let host_calls = self.host_calls();
        let arena = &mut *self.arena;
        arena.retire_layout_tree_update_marks_of_cleared_node(node);

        let row = arena.bound_row(node);
        if !row.is_invalid() {
            // A resource box hangs under the element that references it rather than at its own DOM
            // position.
            if node_facts::kind_is_svg_resource_box(arena.data(row).kind.get())
                && svg_resource_box_survives(arena, row, cleared_subtree_root)
            {
                return true;
            }
            crate::painting::ffi::paintable_cleared_from_node(host_calls, arena, row);
            super::layout_node_arena::prepare_row_for_detach(host_calls, arena, row);
            arena.unbind_row(row);
            let parent = arena.data(row).parent.get();
            if !parent.is_invalid() {
                assert!(arena.detach_from_parent(row));
                host_calls.free_subtree(arena, row);
                // The parent may keep its subtree (a child lost its box in place); an emptied
                // container reads as having block-level children, like a freshly built one.
                if arena.data(parent).first_child.get().is_invalid() {
                    arena.set_node_flag(parent, NodeFlag::ChildrenAreInline, false);
                }
            }
        }

        if node.element_index().is_some() {
            clear_synthetic_pseudo_element_boxes(host_calls, arena, node);
        }
        false
    }
}

/// The box whose child list holds the entry `entry` (see [`LayoutNodeArena::list_entry_top_row`]): its parent, past the
/// anonymous boxes the list made, or none where it has no parent.
pub(super) fn list_box_of_entry(arena: &LayoutNodeArena, entry: NodeSlotId) -> NodeSlotId {
    let mut parent = arena.data(entry).parent.get();
    while !parent.is_invalid() && arena.is_list_closure_wrapper(parent) {
        parent = arena.data(parent).parent.get();
    }
    parent
}

/// Notes the entries that the nodes `nodes` had in the child lists of boxes that stay, for the build that plans those
/// lists again: `nodes` are a node that leaves the document and its shadow-including descendants, in tree order. The
/// box of a node holds the boxes of its descendants, so a node with a box had one entry, that box. A node without one,
/// such as a `display: contents` element, had those of its descendants whose list's box goes with none of the nodes.
pub(crate) fn note_removed_list_entries(arena: &mut LayoutNodeArena, nodes: &[u32]) {
    let attached_entry = |arena: &LayoutNodeArena, node: StyleNodeID| {
        let row = arena.bound_row(node);
        (!row.is_invalid() && !arena.data(row).parent.get().is_invalid()).then(|| arena.list_entry_top_row(row))
    };
    let mut nodes = nodes.iter().filter_map(|&node| StyleNodeID::from_raw(node));
    let Some(removed_node) = nodes.next() else {
        return;
    };
    if let Some(entry) = attached_entry(arena, removed_node) {
        arena.layout_tree_update_marks_mut().note_removed_entry(entry);
        return;
    }
    let mut removed: Option<HashSet<StyleNodeID>> = None;
    for node in nodes.clone() {
        let Some(entry) = attached_entry(arena, node) else {
            continue;
        };
        let removed = removed.get_or_insert_with(|| nodes.clone().chain([removed_node]).collect());
        let list_box = list_box_of_entry(arena, entry);
        if arena
            .dom_node_style_node(list_box)
            .is_some_and(|owner| removed.contains(&owner))
        {
            continue;
        }
        arena.layout_tree_update_marks_mut().note_removed_entry(entry);
    }
}

/// Detaches what is left of the boxes of the node `node` names as the node leaves the document, while its identity
/// still names them. Its box is read until the parent's rebuild frees it, so its style record is pinned and its
/// committed box cleared now. Its box's top layer placement is a viewport child rather than part of the parent's box
/// subtree, so the parent's rebuild never reaches it, and it is detached and freed here. The rows are found by
/// identity, so this makes no shell.
pub(crate) fn detach_remaining_rows_for_removal(
    host_calls: HostCalls<'_>,
    arena: &mut LayoutNodeArena,
    node: StyleNodeID,
) {
    // A pseudo-element's boxes are found through its generator's identity, so they go while the identity still finds
    // them. A ::backdrop box sits outside the generator's box, so no rebuild of the parent would free it.
    if node.element_index().is_some() {
        // NB: Counters and quotes after the node's boxes move with the counters and quotes its pseudo-elements change,
        //     which only a build of the whole tree finds again.
        let pseudo_elements_change_generated_content =
            [GENERATED_FOR_BEFORE, GENERATED_FOR_MARKER, GENERATED_FOR_AFTER]
                .into_iter()
                .any(|generated_for| {
                    let row = arena.bound_pseudo_element_row(node, generated_for);
                    !row.is_invalid()
                        && node_facts::node_style_view(arena.data(row))
                            .is_some_and(|style| !style.counter_properties_are_none())
                });
        if pseudo_elements_change_generated_content
            || crate::layout::quote_depth::pseudo_element_boxes_move_quote_depth(arena, node)
        {
            arena.set_needs_full_layout_tree_update(true);
        }
        clear_synthetic_pseudo_element_boxes(host_calls, arena, node);
    }
    let row = arena.bound_row(node);
    if row.is_invalid() {
        return;
    }
    arena.pin_style_record_for_detachment(row);
    crate::painting::ffi::paintable_cleared_from_node_leaving_ancestors(host_calls, arena, row);
    let top_layer_placement = topmost_layout_node_of_top_layer_placement(arena, row);
    if !top_layer_placement.is_invalid() {
        super::layout_node_arena::prepare_subtree_for_detach(host_calls, arena, top_layer_placement);
        let was_attached = arena.detach_from_parent(top_layer_placement);
        assert!(was_attached, "a top layer placement is a viewport child");
        host_calls.free_subtree(arena, top_layer_placement);
    }
}

pub(crate) fn clear_synthetic_pseudo_element_boxes(
    host_calls: HostCalls<'_>,
    arena: &mut LayoutNodeArena,
    node: StyleNodeID,
) {
    if !arena.has_bound_pseudo_element_rows(node) {
        return;
    }
    for generated_for in GENERATED_FOR_AFTER..=crate::layout::node_data::GENERATED_FOR_LAST_SYNTHETIC {
        free_pseudo_element_box(host_calls, arena, node, generated_for);
    }
}

/// The pseudo-element of kind `generated_for` on the element `node` gives up its box, subtree and
/// all.
fn free_pseudo_element_box(
    host_calls: HostCalls<'_>,
    arena: &mut LayoutNodeArena,
    node: StyleNodeID,
    generated_for: u8,
) {
    let row = arena.bound_pseudo_element_row(node, generated_for);
    if row.is_invalid() {
        return;
    }
    let mut rows = Vec::new();
    arena.for_each_node_in_layout_subtree_in_pre_order(row, |row| rows.push(row));
    for row in rows {
        crate::painting::ffi::paintable_cleared_from_node(host_calls, arena, row);
    }
    super::layout_node_arena::prepare_subtree_for_detach(host_calls, arena, row);
    arena.detach_from_parent(row);
    host_calls.free_subtree(arena, row);
}

/// Clears every stale layout node in the shadow-including subtree `root` names.
///
/// A DOM walk visits a node, then its shadow root's subtree, then its DOM children. A node the
/// style mirror has not named holds no layout tree update mark and can have no box, so navigating
/// the mirror's DOM child sequence reaches everything such a walk had work for, in the same order.
fn clear_stale_subtree(host: &mut TreeBuilderHost, root: StyleNodeID, scope: StaleSubtreeScope) {
    if scope == StaleSubtreeScope::NodeOnly {
        host.clear_stale_layout_node(root, None);
        return;
    }
    let cleared_subtree_root = (scope != StaleSubtreeScope::Inclusive).then_some(root);
    let facts = host.arena().stale_walk_facts(root);
    if scope == StaleSubtreeScope::DescendantsBoundedToRoot {
        clear_stale_subtree_descendants(host, facts, root, cleared_subtree_root);
    } else {
        clear_stale_node(host, root, facts, root, cleared_subtree_root);
    }
}

fn clear_stale_node(
    host: &mut TreeBuilderHost,
    node: StyleNodeID,
    facts: StaleWalkFacts,
    subtree_root: StyleNodeID,
    cleared_subtree_root: Option<StyleNodeID>,
) {
    // A top layer member lays out as a sibling of the root element, so its boxes are not this
    // subtree's to clear.
    if node != subtree_root && facts.rendered_in_top_layer {
        return;
    }
    if host.clear_stale_layout_node(node, cleared_subtree_root) {
        return;
    }
    clear_stale_subtree_descendants(host, facts, subtree_root, cleared_subtree_root);
}

/// Walks below a node whose own facts the caller already read. Clearing a node's box never moves a
/// node, so each child's own step carries where the walk goes after it.
fn clear_stale_subtree_descendants(
    host: &mut TreeBuilderHost,
    facts: StaleWalkFacts,
    subtree_root: StyleNodeID,
    cleared_subtree_root: Option<StyleNodeID>,
) {
    if let Some(shadow_root) = facts.shadow_root {
        let shadow_root_facts = host.arena().stale_walk_facts(shadow_root);
        clear_stale_node(host, shadow_root, shadow_root_facts, subtree_root, cleared_subtree_root);
    }
    let mut child = facts.first_dom_child;
    while let Some(current) = child {
        let child_facts = host.arena().stale_walk_facts(current);
        clear_stale_node(host, current, child_facts, subtree_root, cleared_subtree_root);
        child = child_facts.next_dom_sibling;
    }
}

/// What a layout tree build leaves for its caller: the viewport the tree hangs from. The rebuilt
/// subtree roots wait in the arena for the partial relayout plan that follows the build.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutTreeBuildOutcome {
    pub viewport: NodeSlotId,
    pub needs_another_build_pass: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// NB: `Other` is constructed by C++ through the FFI.
pub enum FfiPseudoElement {
    Before,
    After,
    Marker,
    Backdrop,
    // Marks counter resolution against the element itself rather than one of its pseudo-elements.
    None,
}

/// What a record's `content` computes to: one of the two keywords the property takes, or a list.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ComputedContentType {
    Normal,
    None,
    List,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum PseudoElementDecision {
    None,
    ContentReplacement,
    Contents,
    Box,
}

/// What the build knows about a pseudo-element when it decides whether the pseudo-element gets a box.
#[derive(Clone, Copy)]
pub struct PseudoElementFacts {
    pub has_style: bool,
    pub pseudo_element: FfiPseudoElement,
    pub content_type: ComputedContentType,
    pub display_is_none: bool,
    pub display_is_contents: bool,
    pub display_is_list_item: bool,
    pub has_content_replacement: bool,
    pub normal_marker_has_content: bool,
    pub marker_position_is_inside: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiGeneratedImageKind {
    /// An `<image>` in the pseudo-element's `content` list.
    ContentImage,
    /// A marker box's `list-style-image`.
    ListStyleImage,
}

/// The image a generated image box shows.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiGeneratedImage {
    pub kind: FfiGeneratedImageKind,
    /// For `ContentImage`: the image's index in the `content` list.
    pub content_index: usize,
    /// For `ListStyleImage`: the marker box whose `list-style-image` it is.
    pub marker: NodeSlotId,
}

/// Whether the pseudo-element `facts` describe gets a box, and of what sort; a `::marker` whose originating element's box
/// is a list item box when `has_originating_list_box`.
pub(crate) fn pseudo_element_decision(
    facts: PseudoElementFacts,
    has_originating_list_box: bool,
) -> PseudoElementDecision {
    if !facts.has_style {
        return PseudoElementDecision::None;
    }

    // https://drafts.csswg.org/css-display-3/#box-generation
    // The element and its descendants generate no boxes or text sequences.
    if facts.display_is_none {
        return PseudoElementDecision::None;
    }

    // ::before and ::after only exist if they have content. `content: normal` computes to `none` for them.
    if matches!(facts.pseudo_element, FfiPseudoElement::Before | FfiPseudoElement::After)
        && matches!(
            facts.content_type,
            ComputedContentType::Normal | ComputedContentType::None
        )
    {
        return PseudoElementDecision::None;
    }

    // For ::marker with content 'none' -- do nothing.
    if facts.pseudo_element == FfiPseudoElement::Marker && facts.content_type == ComputedContentType::None {
        return PseudoElementDecision::None;
    }

    if facts.pseudo_element == FfiPseudoElement::Marker
        && facts.content_type == ComputedContentType::Normal
        && has_originating_list_box
    {
        // https://www.w3.org/TR/css-lists-3/#content-property
        // "::marker does not generate a box" when list-style-type is 'none' and there's no marker image. Custom
        // ::marker content is already excluded by the outer condition checking for Type::Normal.
        return if facts.normal_marker_has_content {
            PseudoElementDecision::Box
        } else {
            PseudoElementDecision::None
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
        PseudoElementDecision::ContentReplacement
    } else if facts.display_is_contents {
        PseudoElementDecision::Contents
    } else {
        PseudoElementDecision::Box
    }
}

/// Resolves the CSS counters set of `element`, or of one of its pseudo-elements, now that its box
/// is in the layout tree, and answers whose set it is.
fn resolve_counters(
    host: &mut TreeBuilderHost<'_>,
    element: StyleNodeID,
    pseudo_element: FfiPseudoElement,
) -> crate::layout::counters::CounterOwner {
    let owner = counter_owner_of_pseudo_element(element, pseudo_element);
    crate::layout::counters::resolve_counters(host.arena_mut(), owner);
    owner
}

/// The owner of the counters set and generated content of `element`, or of one of its box-generating
/// pseudo-elements.
fn counter_owner_of_pseudo_element(
    element: StyleNodeID,
    pseudo_element: FfiPseudoElement,
) -> crate::layout::counters::CounterOwner {
    let generated_for = match pseudo_element {
        FfiPseudoElement::None => 0,
        _ => generated_for_of(pseudo_element),
    };
    crate::layout::counters::CounterOwner { element, generated_for }
}

/// The pseudo-element a box-generating `FfiPseudoElement` names, as a row's `generated_for`.
fn generated_for_of(pseudo_element: FfiPseudoElement) -> u8 {
    match pseudo_element {
        FfiPseudoElement::Before => GENERATED_FOR_BEFORE,
        FfiPseudoElement::After => GENERATED_FOR_AFTER,
        FfiPseudoElement::Marker => GENERATED_FOR_MARKER,
        FfiPseudoElement::Backdrop => GENERATED_FOR_BACKDROP,
        FfiPseudoElement::None => unreachable!("only a box-generating pseudo-element has a box"),
    }
}

/// The row the box of kind `kind` of the pseudo-element of kind `generated_for` of `generator` is built in, which the
/// plan decided as `decision`, stamped from the record the style mirror published for the pseudo-element.
fn stamp_pseudo_element_box_row(
    host: &mut TreeBuilderHost<'_>,
    kind: NodeKind,
    generator: StyleNodeID,
    generated_for: u8,
    decision: PseudoElementDecision,
) -> NodeSlotId {
    let slot = host.arena.allocate_unbound();
    host.arena_mut()
        .stamp_pseudo_element_row(slot, kind, generator, generated_for);
    host.write_used_display(slot);
    host.note_style_of_built_row(slot, None);
    host.arena_mut()
        .stamp_pseudo_element_box(slot, generator, generated_for);
    host.arena_mut().owe_image_resources(
        slot,
        OwedImageResources::StyleResources {
            owns_content_replacement_image: decision == PseudoElementDecision::ContentReplacement,
        },
    );
    slot
}

/// The row the list marker a list-item pseudo-element nests is built in, which the plan decided it nests. The marker
/// belongs to the pseudo-element that nests it, not to the generator's own `::marker`, so it is generated for that
/// pseudo-element and never becomes the `::marker`'s box.
fn stamp_nested_list_marker_row(
    host: &mut TreeBuilderHost<'_>,
    generator: StyleNodeID,
    pseudo_element: FfiPseudoElement,
) -> NodeSlotId {
    let derived = host.arena_mut().with_style_engine(|engine| {
        // The generator's own `::marker` record, which the engine derives for this read alone where the generator
        // holds none. It answers one for every list item; should it not, the marker takes the generator's style.
        let record = engine
            .pseudo_published_style_record(generator, pseudo_kind_of(GENERATED_FOR_MARKER))
            .or_else(|| {
                let demand = RecordDemand::PseudoElement(
                    FfiPseudoElementRecordDemand::ReadOnly,
                    FfiDemandedPseudoElement::Marker,
                    StyleAtomID::NONE,
                );
                let record = answer_record_demand(engine, generator, demand).record.style_record;
                (record != 0).then_some(record)
            })
            .or_else(|| engine.element_published_style_record(generator))
            .expect("a list item the build reaches has published its style");
        // NB: Republishing the generator's own `::marker` record can retire its animation record while the nested
        //     marker still refers to it. The marker takes a record of its own, copied from it.
        LayoutStyle::from_record(engine, record).intern(engine)
    });
    let slot = host.arena.allocate_unbound();
    host.arena_mut()
        .stamp_anonymous_box(slot, NodeKind::ListItemMarkerBox, derived);
    host.arena_mut()
        .set_node_generated_for(slot, generated_for_of(pseudo_element), Some(generator));
    host.note_style_of_built_row(slot, None);
    host.arena_mut().owe_image_resources(
        slot,
        OwedImageResources::StyleResources {
            owns_content_replacement_image: false,
        },
    );
    slot
}

/// The row an image of a pseudo-element's generated content is built in, an inline box that takes the style of
/// `style_box`, the pseudo-element's box or the marker it nests.
fn create_generated_image(
    host: &mut TreeBuilderHost<'_>,
    generator: StyleNodeID,
    pseudo_element: FfiPseudoElement,
    image: crate::layout::tree_plan::GeneratedImage,
    style_box: NodeSlotId,
) -> NodeSlotId {
    let image = match image {
        crate::layout::tree_plan::GeneratedImage::Content(content_index) => FfiGeneratedImage {
            kind: FfiGeneratedImageKind::ContentImage,
            content_index: content_index as usize,
            marker: NodeSlotId::INVALID,
        },
        crate::layout::tree_plan::GeneratedImage::ListStyleImage => FfiGeneratedImage {
            kind: FfiGeneratedImageKind::ListStyleImage,
            content_index: 0,
            marker: style_box,
        },
    };
    // https://drafts.csswg.org/css-content-3/#content-property
    // For <image>, this is an inline anonymous replaced element.
    let style_box_record = host.arena().node_style_record(style_box);
    let derived = host.arena_mut().derive_style_record_with_display(
        style_box_record,
        FfiDisplay::outside_and_inside(
            crate::css::css_enums::display_outside::INLINE,
            crate::css::css_enums::display_inside::FLOW,
            false,
        ),
    );
    let slot = host.arena.allocate_unbound();
    host.arena_mut().stamp_anonymous_box(slot, NodeKind::ImageBox, derived);
    host.arena_mut()
        .set_node_generated_for(slot, generated_for_of(pseudo_element), Some(generator));
    host.arena_mut().owe_image_resources(
        slot,
        OwedImageResources::GeneratedImage {
            generator,
            pseudo_element,
            image,
        },
    );
    slot
}

#[derive(Clone, Copy)]
pub struct FirstLetterTarget {
    pub text_layout_node: NodeSlotId,
    pub letter_end: usize,
    pub source_length: usize,
    pub found: bool,
    blocked_by_preserved_line_break: bool,
}

impl FirstLetterTarget {
    fn not_found() -> Self {
        Self {
            text_layout_node: NodeSlotId::INVALID,
            letter_end: 0,
            source_length: 0,
            found: false,
            blocked_by_preserved_line_break: false,
        }
    }
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
    safe fn ladybird_layout_code_point_category_facts(code_point: u32) -> FfiCodePointCategoryFacts;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TraversalDecision {
    Continue,
    SkipChildrenAndContinue,
    Break,
}

/// The tree build's view of the document: the arena it builds rows in, and the host work it owes
/// for what it changes. It holds no main thread token, so it cannot call the host for that work
/// itself.
struct TreeBuilderHost<'a> {
    arena: &'a mut LayoutNodeArena,
    work: &'a OwedHostWork,
}

/// Whether a row of this kind is a `Layout::NodeWithStyle`. A text row is not: it follows its
/// parent's style rather than holding a record of its own, so a reader that wants the row's own
/// box values must skip it.
pub(crate) fn node_kind_is_node_with_style(kind: NodeKind) -> bool {
    !node_facts::kind_is_text(kind) && !matches!(kind, NodeKind::Unset | NodeKind::Node)
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_node_kind_is_replaced_box(kind: NodeKind) -> bool {
    node_facts::kind_is_replaced_box(kind)
}

/// Whether a box of `kind`, built for an element whose style was `built_for`, can show the style `style` as it is. A box
/// whose kind its display decided can't show a display that asks for another kind of box: the layout tree update that
/// the display change marks replaces it, and until then it keeps the style it was built for.
///
/// # Safety
/// Each of `built_for` and `style` is null or the groups of a style record the caller keeps alive for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_box_can_show_style(
    kind: NodeKind,
    built_for: *const std::ffi::c_void,
    style: *const std::ffi::c_void,
) -> bool {
    use super::node_data::LayoutStyleRef;
    // SAFETY: Guaranteed by the caller.
    let (Some(built_for), Some(style)) = (unsafe { LayoutStyleRef::from_raw(built_for) }, unsafe {
        LayoutStyleRef::from_raw(style)
    }) else {
        return true;
    };
    // SAFETY: Guaranteed by the caller.
    let (built_for, style) = unsafe { (built_for.view().display(), style.view().display()) };
    node_kind_for_display(built_for) != Some(kind) || node_kind_for_display(style) == Some(kind)
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_node_kind_is_svg_box(kind: NodeKind) -> bool {
    node_facts::kind_is_svg_box(kind)
}

fn node_is_inline_outside(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    node_facts::kind_is_text(host.data(node).kind.get())
        || host
            .style(node)
            .is_some_and(|style| style.display().is_inline_outside())
}

fn node_is_out_of_flow(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    node_facts::node_is_out_of_flow(host.data(node), host.style(node))
}

fn node_is_fragmented_inline(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    let data = host.data(node);
    node_facts::node_is_fragmented_inline(data, host.style(node))
}

impl<'a> TreeBuilderHost<'a> {
    /// Whether the element this row was built for has a `::first-letter` style.
    fn has_first_letter_style(&self, node: LayoutNode) -> bool {
        let arena = self.arena();
        arena.has_published_first_letter_style(arena.node_style_node(node))
    }

    fn data(&self, node: LayoutNode) -> &NodeData {
        assert!(!node.is_invalid());
        self.arena.data(node)
    }

    fn style(&self, node: LayoutNode) -> Option<ComputedValuesView<'_>> {
        assert!(!node.is_invalid());
        self.arena.style_view(node)
    }

    fn display(&self, node: LayoutNode) -> FfiDisplay {
        self.style(node).map_or_else(FfiDisplay::block, |style| style.display())
    }

    fn display_before_box_type_transformation(&self, node: LayoutNode) -> FfiDisplay {
        self.style(node).map_or_else(FfiDisplay::block, |style| {
            style.display_before_box_type_transformation()
        })
    }

    fn set_children_are_inline(&mut self, node: LayoutNode, children_are_inline: bool) {
        self.arena_mut()
            .set_node_flag(node, NodeFlag::ChildrenAreInline, children_are_inline);
    }

    fn arena(&self) -> &LayoutNodeArena {
        self.arena
    }

    fn arena_mut(&mut self) -> &mut LayoutNodeArena {
        self.arena
    }

    fn created(&self, slot: NodeSlotId) -> UnplacedLayoutNode {
        UnplacedLayoutNode::new(slot)
    }

    /// Gives the row of `node` the display its box uses, see [`used_display_of_box`].
    fn write_used_display(&mut self, node: NodeSlotId) {
        let Some(display) = self.style(node).map(|style| style.display()) else {
            return;
        };
        let data = self.data(node);
        let used_display = used_display_of_box(data.kind.get(), data.flags.get(), display);
        if used_display != display {
            let host_calls = self.host_calls();
            self.arena_mut().update_layout_style(host_calls, node, |style, _| {
                style.set_display(used_display);
            });
        }
    }

    /// The row an element's principal box of `kind` is built in, stamped with `record`, the style
    /// record the element published, and the display its box uses.
    fn create_element_box(&mut self, element: StyleNodeID, kind: NodeKind, record: u64) -> NodeSlotId {
        let slot = self.stamp_dom_box(kind, Some(element));
        self.arena_mut().stamp_published_style(slot, record);
        if kind == NodeKind::ImageBox {
            self.arena_mut()
                .paint_state_mut()
                .bump_custom_highlight_row_generation();
        }
        self.write_used_display(slot);
        self.note_style_of_built_row(slot, Some(element));
        slot
    }

    /// The row a piece of generated text is rendered from, which names no DOM node and carries no
    /// style of its own.
    fn stamp_generated_text_box(&mut self, text: &[u16]) -> NodeSlotId {
        let slot = self.arena.allocate_unbound();
        self.arena
            .stamp_generated_text_row(slot, ak::Utf16String::from_utf16(text));
        slot
    }

    /// Stamps a row for a DOM node, or for the document with no identity, and makes it the node's
    /// row.
    fn stamp_dom_box(&mut self, kind: NodeKind, style_node: Option<StyleNodeID>) -> NodeSlotId {
        let slot = self.arena.allocate_unbound();
        self.arena_mut().stamp_dom_row(slot, kind, style_node);
        self.arena_mut().take_over_rows_of_bound_node(slot);
        slot
    }

    /// The row a text node's box is built in, stamped out of the text node's identity.
    fn create_text_box(&mut self, style_node: StyleNodeID) -> NodeSlotId {
        let slot = self.stamp_dom_box(NodeKind::TextNode, Some(style_node));
        self.stamp_text_row_facts(slot, style_node);
        self.arena_mut()
            .paint_state_mut()
            .bump_custom_highlight_row_generation();
        slot
    }

    /// What giving a row its style tells the rest of the document: which scroll containers
    /// snapping may happen in, with the root element's box standing in for the viewport, and what
    /// an element's highlight pseudo-element styles paint highlighted text with. None of it needs
    /// the row's layout node.
    fn note_style_of_built_row(&mut self, slot: NodeSlotId, element: Option<StyleNodeID>) {
        if let Some(element) = element
            && self.holds_highlight_style(element)
        {
            crate::painting::selection::note_built_row_highlight_pseudo_styles(self.arena_mut(), slot, element);
        }
        if self.arena().node_flags(slot) & NodeFlag::IsDocumentElement as u32 != 0 {
            let viewport = self.arena().bound_viewport_row();
            if !viewport.is_invalid() {
                self.arena_mut().note_built_scroll_container(viewport);
            }
        } else if node_facts::kind_and_style_make_scroll_container(self.data(slot).kind.get(), self.style(slot)) {
            self.arena_mut().note_built_scroll_container(slot);
        }
    }

    /// A text row is stamped with whether an empty text produces a line box fragment, which text
    /// controls and editing hosts rely on: the fragment keeps the line box alive with real font
    /// metrics, giving the caret an anchor to paint at and the control its baseline. The document
    /// restamps it when editability changes. Under an element with a highlight pseudo-element style
    /// and no box of its own, the row takes that style's paint facts itself.
    fn stamp_text_row_facts(&mut self, slot: NodeSlotId, text: StyleNodeID) {
        let is_in_user_agent_shadow_tree =
            self.arena().node_flags(slot) & NodeFlag::IsInUserAgentShadowTree as u32 != 0;
        let (produces_line_box_fragment_when_empty, parent_element) = self.arena().with_style_store(|engine| {
            (
                text_produces_line_box_fragment_when_empty(engine, text, is_in_user_agent_shadow_tree),
                engine
                    .text_parent(text)
                    .filter(|parent| parent.element_index().is_some()),
            )
        });
        self.arena_mut().set_node_flag(
            slot,
            NodeFlag::ProducesLineBoxFragmentWhenEmpty,
            produces_line_box_fragment_when_empty,
        );
        if let Some(parent) = parent_element
            && self.holds_highlight_style(parent)
            && self.arena().bound_row(parent).is_invalid()
        {
            crate::painting::selection::note_built_row_highlight_pseudo_styles(self.arena_mut(), slot, parent);
        }
    }

    /// Whether the element published a record for a highlight pseudo-element. An element whose own
    /// rules style no `::selection` still holds one inherited from an ancestor's, which the
    /// published pseudo-element mask does not show.
    fn holds_highlight_style(&self, element: StyleNodeID) -> bool {
        self.arena().with_style_store(|engine| {
            crate::layout::tree_plan::planner::element_holds_highlight_style(engine, element)
        })
    }

    /// The row the document's viewport is built in, stamped out of its kind and the document's
    /// style the build was handed.
    fn create_viewport_box(&mut self, document_style: DerivedStyleRecord) -> NodeSlotId {
        let slot = self.stamp_dom_box(NodeKind::Viewport, None);
        // The viewport is the document's row, painted with what the document published and named
        // by the document's name.
        let document = self
            .arena()
            .document_style_node()
            .expect("a build names the document it lays out");
        self.arena_mut().stamp_dom_paint_facts(slot, document);
        let unique_node_id = self
            .arena()
            .with_style_store(|engine| engine.element_unique_node_id(document));
        self.arena_mut().unique_node_ids_mut().publish(slot, unique_node_id);
        let host_calls = self.host_calls();
        self.arena_mut()
            .apply_reinherited_style_record(host_calls, slot, document_style);
        self.arena_mut().note_built_scroll_container(slot);
        slot
    }

    fn create_anonymous_box(
        &mut self,
        parent: LayoutNode,
        style_kind: AnonymousStyleKind,
        overrides: AnonymousStyleOverrides,
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
        &mut self,
        parent_style_record: u64,
        style_kind: AnonymousStyleKind,
        overrides: AnonymousStyleOverrides,
        node_kind: NodeKind,
    ) -> UnplacedLayoutNode {
        let derived = self
            .arena_mut()
            .derive_anonymous_style_record(parent_style_record, style_kind, overrides);
        let slot = self.arena.allocate_unbound();
        self.arena_mut().stamp_anonymous_box(slot, node_kind, derived);
        self.arena_mut().refresh_insets_use_anchor_functions_flag(slot);
        // An anonymous inline box takes its style from its parent, so it may name images too.
        if node_kind == NodeKind::InlineNode {
            self.arena_mut().owe_image_resources(
                slot,
                OwedImageResources::StyleResources {
                    owns_content_replacement_image: false,
                },
            );
        }
        UnplacedLayoutNode::new(slot)
    }

    fn attach_child(&mut self, parent: LayoutNode, child: UnplacedLayoutNode, before: LayoutNode) {
        self.arena_mut().attach_child(parent, child, before);
    }

    fn move_child(&mut self, child: LayoutNode, new_parent: LayoutNode, before: LayoutNode) {
        self.arena_mut().move_child(child, new_parent, before);
    }

    fn free_unplaced(&mut self, node: UnplacedLayoutNode) {
        self.free_subtree(node.into_slot());
    }

    fn free_subtree(&mut self, node: LayoutNode) {
        self.host_calls().free_subtree(self.arena, node);
    }

    fn host_calls(&self) -> HostCalls<'a> {
        HostCalls(self.work)
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

    /// Visits the subtree `root` heads in pre-order, lending `visit` the host: a visit may change
    /// the tree below the node it visits, as the walk reads where to go next once the visit returns.
    fn for_each_in_inclusive_subtree(
        &mut self,
        root: LayoutNode,
        mut visit: impl FnMut(&mut Self, LayoutNode) -> TraversalDecision,
    ) {
        let mut current = root;
        while !current.is_invalid() {
            let decision = visit(self, current);
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

    fn remove_nodes(&mut self, nodes: &[LayoutNode]) {
        for &node in nodes {
            let parent = self.parent(node);
            assert!(!parent.is_invalid());
            self.arena_mut().detach_child(parent, node);
        }
        for &node in nodes {
            self.free_subtree(node);
        }
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

// https://drafts.csswg.org/css-pseudo-4/#first-letter-pattern
pub(crate) fn find_first_letter_in_text(
    text: &[u16],
    preserves_segment_breaks: bool,
    next_grapheme_boundary: impl Fn(usize) -> usize,
    code_point_facts: impl Fn(u32) -> FfiCodePointCategoryFacts,
) -> FirstLetterTarget {
    // NB: Matches the first-letter text pattern: (P (Zs|P)*)? (L|N|S) ((Zs|P-(Ps|Pd))* (P-(Ps|Pd))?)?

    let code_units = text.len();
    let mut match_start = 0;
    while match_start < code_units {
        let mut cursor = match_start;
        let starting_code_point = code_point_at(text, cursor);

        // When white-space preserves segment breaks, a newline before any letter puts the letter on a later line, so
        // the first formatted line is empty and ::first-letter must not match.
        if preserves_segment_breaks && (starting_code_point == b'\n' as u32 || starting_code_point == b'\r' as u32) {
            return FirstLetterTarget {
                blocked_by_preserved_line_break: true,
                ..FirstLetterTarget::not_found()
            };
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
            return FirstLetterTarget {
                text_layout_node: NodeSlotId::INVALID,
                letter_end: cursor,
                source_length: code_units,
                found: true,
                blocked_by_preserved_line_break: false,
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

        return FirstLetterTarget {
            text_layout_node: NodeSlotId::INVALID,
            letter_end,
            source_length: code_units,
            found: true,
            blocked_by_preserved_line_break: false,
        };
    }
    FirstLetterTarget::not_found()
}

fn find_first_letter_in_layout_text(host: &TreeBuilderHost<'_>, node: LayoutNode) -> FirstLetterTarget {
    // First-letter matching determines source ranges before text transforms are applied, so it reads
    // the published characters rather than the rendered ones.
    let source = host.arena().published_text_source(node, false);
    let text = source.data.to_utf16();
    let segmenter = GraphemeSegmenter::new(&text);
    let preserves_segment_breaks = matches!(
        host.style(host.parent(node))
            .expect("text parent has style")
            .white_space_collapse(),
        white_space_collapse::PRESERVE | white_space_collapse::PRESERVE_BREAKS | white_space_collapse::BREAK_SPACES
    );
    let mut target = find_first_letter_in_text(
        &text,
        preserves_segment_breaks,
        |index| segmenter.next_boundary(index, false).unwrap_or(text.len()),
        |code_point| ladybird_layout_code_point_category_facts(code_point),
    );
    if target.found {
        target.text_layout_node = node;
    }
    target
}

fn create_first_letter_boxes(host: &mut TreeBuilderHost<'_>, element: StyleNodeID, target: FirstLetterTarget) {
    let text_node = target.text_layout_node;
    let slices_a_dom_text_node = host.data(text_node).kind.get() == NodeKind::TextNode;

    // The first-letter and remainder boxes render slices of the same DOM text node. Generated text
    // has no DOM node, and gets plain generated slices of its characters instead.
    let (first_letter_slice, remainder_slice) = if slices_a_dom_text_node {
        // The remainder takes the text node's rows over, and the first letter's slice renders the
        // same node without becoming the row the node is bound to.
        let text = host.arena().node_style_node(text_node);
        let remainder_slice = host.stamp_dom_box(NodeKind::TextNode, text);
        let first_letter_slice = host.arena.allocate_unbound();
        host.arena_mut()
            .stamp_dom_row(first_letter_slice, NodeKind::TextNode, text);
        host.arena_mut()
            .note_rows_share_dom_node(remainder_slice, first_letter_slice);
        (first_letter_slice, remainder_slice)
    } else {
        let source = host.arena().published_text_source(text_node, false).data;
        let source = source.to_utf16();
        let letter_end = target.letter_end.min(source.len());
        (
            host.stamp_generated_text_box(&source[..letter_end]),
            host.stamp_generated_text_box(&source[letter_end..]),
        )
    };
    let text = host.arena().node_style_node(first_letter_slice);
    if let Some(text) = text {
        for slice in [first_letter_slice, remainder_slice] {
            host.stamp_text_row_facts(slice, text);
        }
    }
    let first_letter_slice_slot = first_letter_slice;
    let remainder_slice_slot = remainder_slice;
    let first_letter_slice = host.created(first_letter_slice);
    let remainder_slice = host.created(remainder_slice);

    let derived = text.and_then(|text| host.arena_mut().derive_first_letter_style_record(element, text));
    let wrapper_kind = host
        .arena()
        .with_style_store(|engine| {
            match derived {
                Some(derived) => engine.published_record_view(derived.record),
                None => engine.pseudo_published_style_view(element, pseudo_kind_of(GENERATED_FOR_FIRST_LETTER)),
            }
            .map(|view| view.display())
        })
        .and_then(node_kind_for_display);
    let Some(wrapper_kind) = wrapper_kind else {
        if let Some(derived) = derived {
            host.arena_mut()
                .with_style_engine(|engine| engine.unpin_layout_style_record(derived.record));
        }
        // A `::first-letter` whose display generates no box leaves the text it matched alone.
        host.free_unplaced(first_letter_slice);
        host.free_unplaced(remainder_slice);
        return;
    };
    if slices_a_dom_text_node {
        // Initialize the source ranges before attaching or rendering either slice.
        host.arena.set_first_letter_slices(
            first_letter_slice_slot,
            remainder_slice_slot,
            target.letter_end,
            target.source_length,
        );
    }

    let wrapper_slot = host.arena.allocate_unbound();
    host.arena_mut()
        .stamp_pseudo_element_row(wrapper_slot, wrapper_kind, element, GENERATED_FOR_FIRST_LETTER);
    if let Some(derived) = derived {
        host.arena_mut()
            .replace_arena_pinned_style_record(wrapper_slot, derived);
    }
    if let Some(text) = text {
        host.arena_mut()
            .note_first_letter_parent_style(wrapper_slot, element, text);
    }
    host.note_style_of_built_row(wrapper_slot, None);
    host.arena_mut().owe_image_resources(
        wrapper_slot,
        OwedImageResources::StyleResources {
            owns_content_replacement_image: false,
        },
    );
    host.arena_mut()
        .clear_pseudo_element_box(element, GENERATED_FOR_FIRST_LETTER);
    host.arena_mut()
        .stamp_pseudo_element_box(wrapper_slot, element, GENERATED_FOR_FIRST_LETTER);
    let wrapper = host.created(wrapper_slot);
    let parent = host.parent(text_node);
    assert!(!parent.is_invalid());
    host.set_children_are_inline(wrapper_slot, true);
    host.attach_child(wrapper_slot, first_letter_slice, NodeSlotId::INVALID);
    host.attach_child(parent, wrapper, text_node);
    host.attach_child(parent, remainder_slice, text_node);
    host.arena_mut().detach_child(parent, text_node);
    host.free_subtree(text_node);
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

/// Splits the box of the text node the caret is in at the caret, around an inline box that renders the writing
/// suggestion showing there, so the suggestion takes up space in the line as text would. It's never in the DOM: The
/// slices on either side keep rendering the DOM text, with source ranges that map its offsets as they do for a
/// `::first-letter` split.
fn create_writing_suggestion_boxes(host: &mut TreeBuilderHost<'_>, element: StyleNodeID, layout_node: LayoutNode) {
    let Some(suggestion) = host.arena().writing_suggestion() else {
        return;
    };
    let is_child_text = host.arena().with_style_store(|engine| {
        if engine.text_parent(suggestion.text) != Some(element) {
            return false;
        }
        // Text that a `::first-letter` may already split is left alone, so the two splits never meet.
        let mut ancestor = Some(element);
        while let Some(current) = ancestor {
            if engine.has_published_first_letter_style(current) {
                return false;
            }
            ancestor = engine.parent_element_or_host(current);
        }
        true
    });
    if !is_child_text {
        return;
    }
    let text_node = host.arena().bound_row(suggestion.text);
    if text_node.is_invalid()
        || host.data(text_node).kind.get() != NodeKind::TextNode
        || host.arena().text_has_source_range(text_node)
        || !is_inclusive_layout_ancestor_of(host, layout_node, text_node)
    {
        return;
    }
    let source_length = host
        .arena()
        .published_text_source(text_node, false)
        .data
        .to_utf16()
        .len();
    if suggestion.offset > source_length || suggestion.remainder.is_empty() {
        return;
    }

    // The trailing slice takes over the text node's rows, and the leading slice renders the same node without
    // becoming the row the node is bound to.
    let trailing_slice = host.stamp_dom_box(NodeKind::TextNode, Some(suggestion.text));
    let leading_slice = host.arena.allocate_unbound();
    host.arena_mut()
        .stamp_dom_row(leading_slice, NodeKind::TextNode, Some(suggestion.text));
    host.arena_mut().note_rows_share_dom_node(trailing_slice, leading_slice);
    for slice in [leading_slice, trailing_slice] {
        host.stamp_text_row_facts(slice, suggestion.text);
    }
    host.arena
        .set_writing_suggestion_slices(leading_slice, trailing_slice, suggestion.offset, source_length);
    let leading_slice = host.created(leading_slice);
    let trailing_slice = host.created(trailing_slice);

    let parent = host.parent(text_node);
    assert!(!parent.is_invalid());
    let wrapper = host.create_anonymous_box(
        parent,
        AnonymousStyleKind::WritingSuggestion,
        AnonymousStyleOverrides::default(),
        NodeKind::InlineNode,
    );
    let wrapper_slot = wrapper.slot();
    host.set_children_are_inline(wrapper_slot, true);
    let suggestion_text = host.stamp_generated_text_box(&suggestion.remainder);
    host.attach_child(wrapper_slot, host.created(suggestion_text), NodeSlotId::INVALID);
    host.attach_child(parent, leading_slice, text_node);
    host.attach_child(parent, wrapper, text_node);
    host.attach_child(parent, trailing_slice, text_node);
    host.arena_mut().detach_child(parent, text_node);
    host.free_subtree(text_node);
}

fn is_marker_content(data: &NodeData) -> bool {
    data.kind.get() == NodeKind::ListItemMarkerBox || data.generated_for.get() == GENERATED_FOR_MARKER
}

enum FirstLetterSearch {
    NotFound,
    Blocked,
    Found(FirstLetterTarget),
}

// https://drafts.csswg.org/css-pseudo-4/#first-letter-application
fn find_first_letter_in_block(host: &mut TreeBuilderHost<'_>, block: LayoutNode) -> FirstLetterSearch {
    // NB: This walks a block container's inline descendants looking for the first-letter text. If the block has block
    //     children instead of inline, recurses into each in-flow block child in turn.
    if node_facts::has_flag(host.data(block), NodeFlag::ChildrenAreInline) {
        let mut result = FirstLetterSearch::NotFound;
        let mut is_root = true;
        host.for_each_in_inclusive_subtree(block, |host, node| {
            if is_root {
                is_root = false;
                return TraversalDecision::Continue;
            }
            let data = host.data(node);
            if is_marker_content(data) || node_is_out_of_flow(host, node) {
                return TraversalDecision::SkipChildrenAndContinue;
            }
            if node_facts::kind_is_text(data.kind.get()) {
                let target = find_first_letter_in_layout_text(host, node);
                if target.blocked_by_preserved_line_break {
                    result = FirstLetterSearch::Blocked;
                    return TraversalDecision::Break;
                }
                if target.found {
                    result = FirstLetterSearch::Found(target);
                    return TraversalDecision::Break;
                }
                return TraversalDecision::Continue;
            }
            if node_is_fragmented_inline(host, node) {
                return TraversalDecision::Continue;
            }
            // https://drafts.csswg.org/css-pseudo-4/#first-letter-application
            // if it is not preceded by any other in-flow content (such as images or inline tables) on its line.
            // NB: An atomic inline occupies the first formatted line even when it supplies no first-letter text.
            result = FirstLetterSearch::Blocked;
            TraversalDecision::Break
        });
        return result;
    }

    // We have no inline content of our own but ::first-letter can still apply to text in an in-flow block descendant,
    // so walk into each in-flow block child in document order until one yields a letter.
    let mut child = host.first_child(block);
    while !child.is_invalid() {
        let data = host.data(child);
        let is_anonymous = node_facts::has_flag(data, NodeFlag::Anonymous);
        if is_marker_content(data) || node_is_out_of_flow(host, child) {
            child = host.next_sibling(child);
            continue;
        }
        if !node_facts::kind_is_block_container(data.kind.get()) {
            break;
        }
        // Stop descending if this child block defines its own ::first-letter: the child will style the first letter
        // inside it, so the ancestor's ::first-letter must not also claim the same letter.
        if !is_anonymous && host.has_first_letter_style(child) {
            break;
        }
        match find_first_letter_in_block(host, child) {
            FirstLetterSearch::NotFound => {}
            result => return result,
        }
        if !is_anonymous {
            break;
        }
        child = host.next_sibling(child);
    }
    FirstLetterSearch::NotFound
}

impl crate::layout::tree_plan::table_fixup::TableFixupTree for TreeBuilderHost<'_> {
    fn parent(&self, node: LayoutNode) -> LayoutNode {
        TreeBuilderHost::parent(self, node)
    }

    fn first_child(&self, node: LayoutNode) -> LayoutNode {
        TreeBuilderHost::first_child(self, node)
    }

    fn next_sibling(&self, node: LayoutNode) -> LayoutNode {
        TreeBuilderHost::next_sibling(self, node)
    }

    fn previous_sibling(&self, node: LayoutNode) -> LayoutNode {
        TreeBuilderHost::previous_sibling(self, node)
    }

    fn kind(&self, node: LayoutNode) -> NodeKind {
        self.data(node).kind.get()
    }

    fn has_flag(&self, node: LayoutNode, flag: NodeFlag) -> bool {
        node_facts::has_flag(self.data(node), flag)
    }

    fn is_out_of_flow(&self, node: LayoutNode) -> bool {
        node_is_out_of_flow(self, node)
    }

    fn is_inline_outside(&self, node: LayoutNode) -> bool {
        node_is_inline_outside(self, node)
    }

    fn display(&self, node: LayoutNode) -> FfiDisplay {
        TreeBuilderHost::display(self, node)
    }

    fn display_before_box_type_transformation(&self, node: LayoutNode) -> FfiDisplay {
        TreeBuilderHost::display_before_box_type_transformation(self, node)
    }

    fn text_is_white_space(&mut self, text: LayoutNode) -> bool {
        super::rendered_text::ensure_text_content(self.arena, text);
        self.arena()
            .text_content(text)
            .expect("text was just refreshed")
            .text
            .iter()
            .all(|unit| matches!(unit, 0x09..=0x0d | 0x20))
    }

    fn remove(&mut self, nodes: &[LayoutNode]) {
        self.remove_nodes(nodes);
    }

    fn wrap_in_anonymous_table_box(
        &mut self,
        parent: LayoutNode,
        nodes: &[LayoutNode],
        before: LayoutNode,
        kind: AnonymousTableBoxKind,
    ) -> LayoutNode {
        let (style_kind, node_kind) = kind.style_and_node_kind();
        let wrapper = self.create_anonymous_box(parent, style_kind, AnonymousStyleOverrides::default(), node_kind);
        let wrapper_slot = wrapper.slot();
        for &node in nodes {
            self.move_child(node, wrapper_slot, NodeSlotId::INVALID);
        }
        self.attach_child(parent, wrapper, before);
        wrapper_slot
    }

    fn set_children_are_inline(&mut self, node: LayoutNode, children_are_inline: bool) {
        TreeBuilderHost::set_children_are_inline(self, node, children_are_inline);
    }

    fn wrap_table_root(&mut self, table_root: LayoutNode) {
        let nearest_sibling = TreeBuilderHost::next_sibling(self, table_root);
        let parent = TreeBuilderHost::parent(self, table_root);
        let wrapper = self.create_anonymous_box(
            table_root,
            AnonymousStyleKind::TableWrapper,
            AnonymousStyleOverrides::default(),
            NodeKind::TableWrapper,
        );
        let host_calls = self.host_calls();
        self.arena_mut()
            .reset_table_box_style_used_by_wrapper(host_calls, table_root);
        self.move_child(table_root, wrapper.slot(), NodeSlotId::INVALID);
        self.attach_child(parent, wrapper, nearest_sibling);
    }
}

fn fixup_row(
    host: &mut TreeBuilderHost<'_>,
    row: LayoutNode,
    table_grid: &table_formatting_context::TableGrid,
    row_index: usize,
) {
    let required_cell_count = (0..table_grid.column_count)
        .filter(|&column_index| !table_grid.occupancy.contains(&(column_index, row_index)))
        .count();
    let mut existing_cells = Vec::new();
    let mut missing_cells_are_trailing = true;
    let mut child = host.first_child(row);
    while !child.is_invalid() {
        if node_facts::has_flag(host.data(child), NodeFlag::IsMissingTableCell) {
            existing_cells.push(child);
        } else if !existing_cells.is_empty() {
            missing_cells_are_trailing = false;
        }
        child = host.next_sibling(child);
    }
    // OPTIMIZATION: Preserve trailing anonymous cells that still fill the grid. Recreating
    //               them on every cell-content change invalidates layout and paint caches
    //               for otherwise untouched rows throughout the table.
    let retained_cell_count = if missing_cells_are_trailing {
        existing_cells.len().min(required_cell_count)
    } else {
        0
    };
    host.remove_nodes(&existing_cells[retained_cell_count..]);
    for _ in retained_cell_count..required_cell_count {
        let cell = host.create_anonymous_box(
            row,
            AnonymousStyleKind::MissingTableCell,
            AnonymousStyleOverrides::default(),
            NodeKind::BlockContainer,
        );
        host.arena_mut()
            .set_node_flag(cell.slot(), NodeFlag::IsMissingTableCell, true);
        host.attach_child(row, cell, NodeSlotId::INVALID);
    }
}

fn missing_cells_fixup(host: &mut TreeBuilderHost<'_>, table_roots: &[LayoutNode]) {
    // https://drafts.csswg.org/css-tables-3/#missing-cells-fixup
    // Once the amount of columns in a table is known, any table-row box must be modified such that it owns enough
    // cells to fill all the columns of the table, when taking spans into account. New table-cell anonymous boxes must
    // be appended to its rows content until this condition is met.
    for &table_root in table_roots {
        let grid = table_formatting_context::calculate_table_grid(
            host,
            table_root,
            table_formatting_context::MissingTableCells::Exclude,
        );
        for (row_index, row) in grid.rows.iter().enumerate() {
            fixup_row(host, row.box_, &grid, row_index);
        }
    }
}

fn nearest_table_root(host: &TreeBuilderHost<'_>, node: LayoutNode) -> Option<LayoutNode> {
    let mut current = node;
    while !current.is_invalid() {
        let data = host.data(current);
        if node_facts::has_flag(data, NodeFlag::HasStyle)
            && crate::layout::tree_plan::table_fixup::display_for_table_fixup(host, current).is_table_inside()
        {
            return Some(current);
        }
        current = host.parent(current);
    }
    None
}

/// Fixes up the tables of the child lists of `owners` (see
/// [`crate::layout::tree_plan::table_fixup::fix_up_tables_in_child_lists`]), the lists a build left to the commit
/// where table fixup has anything to do, then the missing cells of `table_roots`, and of every table the steps reached.
fn fixup_tables_in_child_lists(
    host: &mut TreeBuilderHost<'_>,
    owners: &[LayoutNode],
    mut table_roots: Vec<LayoutNode>,
) {
    let mut seen = HashSet::default();
    let owners: Vec<LayoutNode> = owners
        .iter()
        .copied()
        .filter(|&owner| seen.insert(owner) && host.arena().node_data_if_live(owner).is_some())
        .collect();
    let mut known_table_roots: HashSet<LayoutNode> = table_roots.iter().copied().collect();
    for table_root in crate::layout::tree_plan::table_fixup::fix_up_tables_in_child_lists(host, &owners) {
        if known_table_roots.insert(table_root) {
            table_roots.push(table_root);
        }
    }
    missing_cells_fixup(host, &table_roots);
}

#[cfg(test)]
mod tests {
    use crate::css::style::bridge::element_adjustment_fact;
    use crate::layout::node_data::NodeSlotId;
    use crate::layout::tree_builder::{
        ComputedContentType, ElementLayoutKind, FfiCodePointCategoryFacts, FfiPseudoElement, PseudoElementDecision,
        PseudoElementFacts, display_contents_text_needs_style_wrapper, element_layout_kind, find_first_letter_in_text,
        pseudo_element_decision,
    };
    #[test]
    fn clearing_a_stale_node_frees_its_box_and_its_pseudo_element_boxes() {
        use crate::css::style::tree::StyleNodeID;
        use crate::layout::LayoutNodeArena;
        use crate::layout::node_data::{NodeConstructionFacts, NodeFlag, NodeKind};
        use crate::layout::tree_mutation::UnplacedLayoutNode;
        use crate::stage::MainThread;

        let facts = |style_node: Option<StyleNodeID>| NodeConstructionFacts {
            kind: NodeKind::BlockContainer,
            is_anonymous: style_node.is_none(),
            is_html_input_element: false,
            is_html_html_element: false,
            is_document_element: false,
            is_in_user_agent_shadow_tree: false,
            uses_button_layout: false,
            is_editing_host: false,
            is_body: false,
            dom_paint_facts: 0,
            style_node: style_node.map_or(0, StyleNodeID::raw),
        };
        // The rows name their generators, whose unique node ids the style mirror answers for.
        let mut engine = crate::css::style::StyleEngine::new();
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let element = StyleNodeID::element(4);
        let parent = arena.allocate(facts(None));
        let element_box = arena.allocate(facts(Some(element)));
        let before_box = arena.allocate(facts(None));
        arena.set_node_generated_for(before_box, 1, Some(element));
        arena.attach_child(parent, UnplacedLayoutNode::new(element_box), NodeSlotId::INVALID);
        arena.attach_child(element_box, UnplacedLayoutNode::new(before_box), NodeSlotId::INVALID);
        arena.bind_row(element_box);
        arena.bind_row(before_box);
        arena.set_node_flag(parent, NodeFlag::ChildrenAreInline, true);

        let main_thread = MainThread::for_test();
        let work = crate::layout::tree_mutation::OwedHostWork::default();
        arena.queue_box_presence();
        let mut host = super::TreeBuilderHost {
            arena: &mut arena,
            work: &work,
        };
        assert!(!host.clear_stale_layout_node(element, None));
        work.resolve(&mut arena).pay(&main_thread);

        assert!(!arena.slot_is_live(element_box));
        assert!(!arena.slot_is_live(before_box));
        assert!(arena.bound_row(element).is_invalid());
        assert!(arena.bound_pseudo_element_row(element, 1).is_invalid());
        assert!(arena.data(parent).first_child.get().is_invalid());
        assert!(arena.data(parent).flags.get() & NodeFlag::ChildrenAreInline as u32 == 0);
        arena
            .free_subtree(parent)
            .destroy_shells_and_invoke_callbacks(&main_thread);
    }

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
    ) -> crate::layout::tree_builder::FirstLetterTarget {
        let text = text.encode_utf16().collect::<Vec<_>>();
        find_first_letter_in_text(&text, preserves_segment_breaks, |index| index + 1, code_point_facts)
    }

    #[test]
    fn preserved_break_in_an_earlier_text_node_blocks_first_letter_search() {
        use crate::css::css_enums::white_space_collapse;
        use crate::css::style::layout_style::LayoutScratchStyle;
        use crate::layout::layout_node_arena::LayoutNodeArena;
        use crate::layout::node_data::{NodeFlag, NodeKind};
        use crate::layout::tree_mutation::OwedHostWork;

        for (prefix, collapse, blocked) in [
            ("\n", white_space_collapse::PRESERVE, true),
            ("  \n", white_space_collapse::PRESERVE_BREAKS, true),
            ("\r", white_space_collapse::BREAK_SPACES, true),
            ("  ", white_space_collapse::PRESERVE, false),
            ("\n", white_space_collapse::COLLAPSE, false),
        ] {
            let style = LayoutScratchStyle::over_defaults(|style| style.set_white_space_collapse(collapse));
            let mut arena = LayoutNodeArena::new();
            let block = arena.allocate_for_test().slot;
            arena.write_shape(block).set_kind(NodeKind::BlockContainer);
            arena.set_node_flag(block, NodeFlag::ChildrenAreInline, true);
            arena.set_node_style(block, 0, Some(style.style()));
            let first = arena.allocate_unbound();
            let second = arena.allocate_unbound();
            arena.stamp_generated_text_row(
                first,
                ak::Utf16String::from_utf16(&prefix.encode_utf16().collect::<Vec<_>>()),
            );
            arena.stamp_generated_text_row(
                second,
                ak::Utf16String::from_utf16(&"Text".encode_utf16().collect::<Vec<_>>()),
            );
            arena.insert_child(block, first, NodeSlotId::INVALID);
            arena.insert_child(block, second, NodeSlotId::INVALID);
            let work = OwedHostWork::default();
            let mut host = super::TreeBuilderHost {
                arena: &mut arena,
                work: &work,
            };
            let result = super::find_first_letter_in_block(&mut host, block);
            if blocked {
                assert!(
                    matches!(result, super::FirstLetterSearch::Blocked),
                    "preserved break must stop the actual subtree walk"
                );
            } else {
                assert!(matches!(result, super::FirstLetterSearch::Found(target) if target.text_layout_node == second));
            }
            arena
                .free_subtree(block)
                .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        }
    }

    #[test]
    fn first_letter_text_pattern() {
        let target = first_letter_target("  Hello", false);
        assert!(target.found);
        assert_eq!(target.letter_end, 3);

        let target = first_letter_target("\") A", false);
        assert!(target.found);
        assert_eq!(target.letter_end, 4);

        let target = first_letter_target("H!ello", false);
        assert!(target.found);
        assert_eq!(target.letter_end, 2);

        let target = first_letter_target("H-ello", false);
        assert!(target.found);
        assert_eq!(target.letter_end, 1);

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
        assert_eq!((target.letter_end, target.source_length), (4, 7));

        let text: Vec<u16> = "I\u{0307}abc".encode_utf16().collect();
        let target = find_first_letter_in_text(
            &text,
            false,
            |index| if index == 0 { 2 } else { index + 1 },
            code_point_facts,
        );
        assert!(target.found);
        assert_eq!(target.letter_end, 2);
    }

    #[test]
    fn pseudo_element_box_generation_decisions() {
        let decide = |pseudo_element,
                      content_type,
                      display_is_none,
                      display_is_contents,
                      display_is_list_item,
                      has_content_replacement,
                      originating_layout_node_is_list_item: bool,
                      normal_marker_has_content| {
            pseudo_element_decision(
                PseudoElementFacts {
                    has_style: true,
                    pseudo_element,
                    content_type,
                    display_is_none,
                    display_is_contents,
                    display_is_list_item,
                    has_content_replacement,
                    normal_marker_has_content,
                    marker_position_is_inside: false,
                },
                originating_layout_node_is_list_item,
            )
        };

        assert_eq!(
            decide(
                FfiPseudoElement::Before,
                ComputedContentType::Normal,
                false,
                false,
                false,
                false,
                false,
                false
            ),
            PseudoElementDecision::None
        );
        assert_eq!(
            decide(
                FfiPseudoElement::Marker,
                ComputedContentType::Normal,
                false,
                false,
                false,
                false,
                true,
                true
            ),
            PseudoElementDecision::Box
        );
        assert_eq!(
            decide(
                FfiPseudoElement::Backdrop,
                ComputedContentType::List,
                false,
                false,
                false,
                true,
                false,
                false
            ),
            PseudoElementDecision::ContentReplacement
        );
        assert_eq!(
            decide(
                FfiPseudoElement::Backdrop,
                ComputedContentType::List,
                false,
                true,
                false,
                true,
                false,
                false
            ),
            PseudoElementDecision::Contents
        );
        assert_eq!(
            decide(
                FfiPseudoElement::Backdrop,
                ComputedContentType::List,
                false,
                false,
                true,
                true,
                false,
                false
            ),
            PseudoElementDecision::Box
        );
    }

    #[test]
    fn specialized_element_layout_kinds() {
        assert_eq!(element_layout_kind(false, 0, false, false), ElementLayoutKind::Normal);
        assert_eq!(
            element_layout_kind(true, 0, false, false),
            ElementLayoutKind::ContentReplacement
        );
        assert_eq!(
            element_layout_kind(false, element_adjustment_fact::IS_SVG_MASK_ELEMENT, true, false),
            ElementLayoutKind::SvgMask
        );
        assert_eq!(
            element_layout_kind(false, element_adjustment_fact::IS_SVG_PATTERN_ELEMENT, false, true),
            ElementLayoutKind::SvgPattern
        );
    }

    #[test]
    fn display_contents_text_style_wrapper_decisions() {
        assert!(!display_contents_text_needs_style_wrapper(false, true, false, false));
        assert!(display_contents_text_needs_style_wrapper(true, true, false, true));
        assert!(!display_contents_text_needs_style_wrapper(true, true, true, true));
        assert!(display_contents_text_needs_style_wrapper(true, true, true, false));
    }
}
