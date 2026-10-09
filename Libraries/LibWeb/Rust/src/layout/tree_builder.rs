/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

use crate::css::css_enums::{content_visibility, float, positioning, white_space_collapse};
use crate::css::style::RecordDemand;
use crate::css::style::StyleEngine;
use crate::css::style::bridge::{
    ElementBoxKind, FfiDemandedPseudoElement, FfiPseudoElementRecordDemand, answer_record_demand,
    element_adjustment_fact,
};
use crate::css::style::layout_style::{AnonymousStyleKind, AnonymousStyleOverrides, DerivedStyleRecord, LayoutStyle};
use crate::css::style::tree::StyleNodeID;
use crate::layout::layout_node_arena::{LayoutNodeArena, OwedImageResources, StaleWalkFacts};
use crate::layout::node_data::{
    GENERATED_FOR_AFTER, GENERATED_FOR_BACKDROP, GENERATED_FOR_BEFORE, GENERATED_FOR_FIRST_LETTER,
    GENERATED_FOR_MARKER, NodeData, NodeFlag, NodeKind, NodeSlotId, pseudo_kind_of,
};
use crate::layout::text_chunker::{GraphemeSegmenter, code_point_at, code_unit_length_for_code_point};
use crate::layout::tree_mutation::{HostCalls, OwedHostWork, UnplacedLayoutNode};
use crate::layout::tree_update_marks::layout_tree_update_reuse_reason;
use crate::layout::{ComputedValuesView, FfiDisplay};
use crate::painting::paint_read::PaintRead;

type LayoutNode = NodeSlotId;

pub(crate) struct TreeBuilderState {
    pub(crate) ancestor_stack: Vec<LayoutNode>,
    pub(crate) quote_nesting_level: u32,
    // Partial-rebuild bookkeeping: boxes replaced in place become rebuild roots, and any tree
    // restructuring that reaches outside every rebuild root downgrades the update to a full one.
    current_rebuild_root: LayoutNode,
    rebuilt_subtree_roots: Vec<LayoutNode>,
    reused_child_list_update_roots: Vec<LayoutNode>,
    additional_table_fixup_roots: Vec<LayoutNode>,
    layout_tree_update_escaped_rebuild_roots: bool,
    new_subtree_root: LayoutNode,
    /// The elements a finished build asks the document to rebuild. `None` asks for the whole tree:
    /// the container the box escaped into stands for no element.
    layout_tree_rebuild_requests: Vec<Option<StyleNodeID>>,
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

impl Default for TreeBuilderState {
    fn default() -> Self {
        Self {
            ancestor_stack: Vec::new(),
            quote_nesting_level: 0,
            current_rebuild_root: NodeSlotId::INVALID,
            rebuilt_subtree_roots: Vec::new(),
            reused_child_list_update_roots: Vec::new(),
            additional_table_fixup_roots: Vec::new(),
            layout_tree_update_escaped_rebuild_roots: false,
            new_subtree_root: NodeSlotId::INVALID,
            layout_tree_rebuild_requests: Vec::new(),
            reached_unstyled_element: false,
            reports: Vec::new(),
            pinned_style_records: Vec::new(),
            document_style: None,
        }
    }
}

impl TreeBuilderState {
    /// Holds `record` until the build ends.
    fn pin_style_record_for_build(&mut self, arena: &LayoutNodeArena, record: u64) {
        arena.with_style_engine(|engine| engine.pin_layout_style_record(record));
        self.pinned_style_records.push(record);
    }

    /// Lets go of every record the build held, once the build is over.
    pub(super) fn release_pinned_style_records(&mut self, arena: &LayoutNodeArena) {
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

#[derive(Default)]
pub(crate) struct TreeBuilderContext {
    pub(crate) has_svg_root: bool,
    pub(crate) layout_top_layer: bool,
    /// The document asked for every box to be recreated, read from the arena once per build.
    pub(crate) document_needs_full_layout_tree_update: bool,
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
enum StaleSubtreeClearScope {
    Inclusive,
    InclusiveBoundedToRoot,
    DescendantsBoundedToRoot,
}

/// What the build knows about a node when it enters it: what its marks ask for, and what layout
/// node it already has. Every field is read out of the arena by identity.
#[derive(Clone, Copy)]
pub(crate) struct PrincipalNodeEntryFacts {
    pub must_create_subtree: bool,
    pub needs_layout_tree_update: bool,
    pub has_layout_node: bool,
    pub layout_node_is_attached: bool,
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

fn apply_replaced_display_adjustment(
    host_calls: HostCalls<'_>,
    arena: &LayoutNodeArena,
    node: NodeSlotId,
    adjustment: FfiReplacedElementDisplayAdjustment,
) {
    use crate::css::css_enums::{display_inside, display_outside};
    let outside = match adjustment {
        FfiReplacedElementDisplayAdjustment::Block => display_outside::BLOCK,
        FfiReplacedElementDisplayAdjustment::Inline => display_outside::INLINE,
        FfiReplacedElementDisplayAdjustment::None => return,
    };
    arena.update_layout_style(host_calls, node, |style| {
        style.set_display(FfiDisplay::outside_and_inside(outside, display_inside::FLOW, false));
    });
}

/// The kind of box a computed display asks for, or none for a display that generates no box.
fn node_kind_for_display(display: FfiDisplay) -> Option<NodeKind> {
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
    if display.is_flow_inside() || display.is_flow_root_inside() || display.is_ruby_inside() {
        return Some(NodeKind::BlockContainer);
    }
    Some(NodeKind::InlineNode)
}

/// The kind of principal box an element asks for. The element's own type and state decided its
/// box kind at style time; its computed display decides the box for the kinds that leave the
/// choice to it, and `appearance: none` suppresses an input's native widget.
/// https://drafts.csswg.org/css-ui/#appearance-switching
fn node_kind_for_element_box_kind(box_kind: ElementBoxKind, display: FfiDisplay, appearance: u8) -> Option<NodeKind> {
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
        ElementBoxKind::NavigableContainerViewport => NodeKind::NavigableContainerViewport,
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
) -> FfiElementLayoutKind {
    let has = |fact: u32| element_type_facts & fact != 0;
    if has_content_replacement {
        FfiElementLayoutKind::ContentReplacement
    } else if layout_svg_mask_or_clip_path {
        if has(element_adjustment_fact::IS_SVG_MASK_ELEMENT) {
            FfiElementLayoutKind::SvgMask
        } else {
            assert!(has(element_adjustment_fact::IS_SVG_CLIP_PATH_ELEMENT));
            FfiElementLayoutKind::SvgClipPath
        }
    } else if layout_svg_pattern {
        assert!(has(element_adjustment_fact::IS_SVG_PATTERN_ELEMENT));
        FfiElementLayoutKind::SvgPattern
    } else {
        FfiElementLayoutKind::Normal
    }
}

/// Which kind of DOM node the walk is standing on.
///
/// A `StyleNodeID` says element or text by itself, and the document is the only other node the walk
/// enters: it is the build's root. A node that can never have a box (a comment, a doctype, a
/// processing instruction) holds no place in the style mirror's child sequence, so the walk never
/// reaches one.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PrincipalNodeKind {
    Document,
    Element,
    Text,
}

impl PrincipalNodeKind {
    fn of(identity: StyleNodeID, is_document_root: bool) -> Self {
        if is_document_root {
            Self::Document
        } else if identity.text_index().is_some() {
            Self::Text
        } else {
            Self::Element
        }
    }

    pub(crate) fn is_document(self) -> bool {
        self == Self::Document
    }

    pub(crate) fn is_element(self) -> bool {
        self == Self::Element
    }

    pub(crate) fn is_text(self) -> bool {
        self == Self::Text
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
    if is_element && display_is_none {
        PrincipalBoxGenerationDecision::Suppress
    } else if is_element && display_is_contents {
        PrincipalBoxGenerationDecision::DisplayContents
    } else {
        PrincipalBoxGenerationDecision::PrincipalBox
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
        let tree = engine.tree();
        // The document holds every node a row stands for.
        let root_is_document = tree.is_relation_only(cleared_subtree_root);
        let mut ancestor = arena.data(layout_node).parent.get();
        while !ancestor.is_invalid() {
            let data = arena.data(ancestor);
            if data.flags.get() & NodeFlag::Anonymous as u32 == 0 {
                let inside = if data.kind.get() == NodeKind::Viewport {
                    root_is_document
                } else {
                    arena.node_style_node(ancestor).is_some_and(|style_node| {
                        root_is_document || tree.is_in_shadow_including_subtree_of(style_node, cleared_subtree_root)
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
}

/// Which way a sibling walk runs.
#[derive(Clone, Copy, PartialEq, Eq)]
enum SiblingDirection {
    Previous,
    Next,
}

/// Whether the box an element already has can take the children that were just inserted under it,
/// rather than being rebuilt around them.
///
/// The test reads the DOM child sequence, the sibling boxes an inserted child would land between,
/// and the style records of children that have no box yet, all out of the style mirror and the
/// arena. Every rejection is a shape the incremental insertion cannot produce the same tree for.
struct ChildListInsertionReuse<'a, 'host> {
    layout: &'a TreeBuilderHost<'host>,
    engine: &'a StyleEngine,
    element: StyleNodeID,
    layout_node: LayoutNode,
    /// Whether a `::first-letter` owner styles the first letter of this subtree, whose source
    /// range only a full build can find again.
    has_first_letter_owner: bool,
    /// The last layout child, when it is an anonymous inline run that an inserted text node would
    /// have to join.
    trailing_inline_wrapper: LayoutNode,
}

/// Which narrower rebuilds the build settled on for one node.
#[derive(Clone, Copy, Default)]
pub(crate) struct LayoutNodeReuse {
    pub insert_children: bool,
    pub update_pseudo_elements: bool,
}

/// Settles which of the narrower rebuilds the node's marks asked for the build can actually take.
/// Neither is available unless every mark the node collected permits it, because a rebuild that
/// only updates the pseudo-elements leaves the child list alone, and the other way round.
fn resolve_layout_node_reuse(
    host: &TreeBuilderHost<'_>,
    kind: PrincipalNodeKind,
    style_node: Option<StyleNodeID>,
) -> LayoutNodeReuse {
    let Some(element) = style_node else {
        return LayoutNodeReuse::default();
    };
    let reasons = host.arena().layout_tree_update_reuse_reasons(element);
    // One borrow of the style mirror answers both tests; each walks the child list several times.
    let (insert_children, update_pseudo_elements) = host.arena().with_style_store(|engine| {
        (
            reasons & layout_tree_update_reuse_reason::CHILD_LIST_INSERTION != 0
                && may_reuse_layout_node_for_child_list_insertion(host, engine, kind, element),
            reasons & layout_tree_update_reuse_reason::PSEUDO_ELEMENT_CHANGE != 0
                && may_update_pseudo_elements_in_place(host, engine, kind, element),
        )
    });
    let may_reuse = (reasons & layout_tree_update_reuse_reason::PSEUDO_ELEMENT_CHANGE == 0 || update_pseudo_elements)
        && (reasons & layout_tree_update_reuse_reason::CHILD_LIST_INSERTION == 0 || insert_children);
    LayoutNodeReuse {
        insert_children: may_reuse && insert_children,
        update_pseudo_elements: may_reuse && update_pseudo_elements,
    }
}

/// Whether a build that finds `element` marked for `reuse_reason` alone, the one narrower rebuild it names, keeps the
/// element's box: it regenerates the box's `::before` and `::after`, or builds only the children inserted under it.
pub(crate) fn build_keeps_box(arena: &mut LayoutNodeArena, element: StyleNodeID, reuse_reason: u8) -> bool {
    let work = OwedHostWork::default();
    // The tests read the arena alone.
    let layout = TreeBuilderHost { arena, work: &work };
    let kind = PrincipalNodeKind::of(element, false);
    layout.arena().with_style_store(|engine| match reuse_reason {
        layout_tree_update_reuse_reason::PSEUDO_ELEMENT_CHANGE => {
            may_update_pseudo_elements_in_place(&layout, engine, kind, element)
        }
        layout_tree_update_reuse_reason::CHILD_LIST_INSERTION => {
            may_reuse_layout_node_for_child_list_insertion(&layout, engine, kind, element)
        }
        _ => false,
    })
}

/// Whether the element's `::before` and `::after` boxes can be regenerated where they sit, rather
/// than the element's box being rebuilt around them.
///
/// The box has to be an ordinary block container holding one inline run, and each pseudo-element
/// has to be content that says nothing about where it ends up: no counter it moves, and either a
/// bare keyword or a list of plain strings. Anything else is content whose value depends on the
/// tree around it, which only a full build resolves.
fn may_update_pseudo_elements_in_place(
    layout: &TreeBuilderHost<'_>,
    engine: &StyleEngine,
    kind: PrincipalNodeKind,
    element: StyleNodeID,
) -> bool {
    let arena = layout.arena();
    let layout_node = arena.bound_row(element);
    if !kind.is_element()
        || layout_node.is_invalid()
        || layout.data(layout_node).kind.get() != NodeKind::BlockContainer
        || engine.tree().shadow_root_of(element).is_some()
        || engine.element_adjustment_facts(element) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0
    {
        return false;
    }

    let Some(facts) = engine.element_published_box_facts(element) else {
        return false;
    };
    if facts.content_visibility != content_visibility::VISIBLE
        || (!facts.display.is_flow_inside() && !facts.display.is_flow_root_inside())
    {
        return false;
    }

    let has_children = !layout.first_child(layout_node).is_invalid();
    if has_children && !node_facts::has_flag(layout.data(layout_node), NodeFlag::ChildrenAreInline) {
        return false;
    }
    let mut child = layout.first_child(layout_node);
    while !child.is_invalid() {
        let data = layout.data(child);
        if node_facts::has_flag(data, NodeFlag::Anonymous) && !node_is_generated_for_pseudo_element(data) {
            return false;
        }
        child = layout.next_sibling(child);
    }

    if first_letter_owner_covers_subtree(layout, engine, element, layout_node) {
        return false;
    }

    for generated_for in [GENERATED_FOR_BEFORE, GENERATED_FOR_AFTER] {
        let old_box = arena.bound_pseudo_element_row(element, generated_for);
        if !old_box.is_invalid() {
            // NB: The old box already holds the new style, including display:none when it is
            //     disappearing.
            let display = layout.display(old_box);
            if layout.parent(old_box) != layout_node
                || (!display.is_inline_outside() && !display.is_none())
                || (node_kind_is_node_with_style(layout.data(old_box).kind.get())
                    && node_is_out_of_flow(layout, old_box))
            {
                return false;
            }
        }

        let pseudo_kind = pseudo_kind_of(generated_for);
        let Some(content) = engine.pseudo_published_content_facts(element, pseudo_kind) else {
            continue;
        };
        if !content.counters_are_none || !(content.content_is_keyword || content.content_is_strings_only) {
            return false;
        }
        let Some(pseudo_facts) = engine.pseudo_published_box_facts(element, pseudo_kind) else {
            continue;
        };
        if pseudo_facts.display.is_none() || content.content_is_keyword {
            continue;
        }
        if !pseudo_facts.display.is_inline_outside()
            || pseudo_facts.display.is_list_item()
            || pseudo_facts.position == positioning::ABSOLUTE
            || pseudo_facts.position == positioning::FIXED
            || pseudo_facts.float_ != float::NONE
        {
            return false;
        }
    }
    true
}

/// Whether a `::first-letter` owner on or above the element styles a letter inside its subtree.
/// See `DOM::Node::first_letter_owner_for_layout_subtree_from`.
fn first_letter_owner_covers_subtree(
    layout: &TreeBuilderHost<'_>,
    engine: &StyleEngine,
    element: StyleNodeID,
    layout_node: LayoutNode,
) -> bool {
    let arena = layout.arena();
    let mut ancestor = Some(element);
    while let Some(current) = ancestor {
        if engine.has_published_first_letter_style(current) {
            let first_letter = arena.bound_pseudo_element_row(current, GENERATED_FOR_FIRST_LETTER);
            if first_letter.is_invalid() || is_inclusive_layout_ancestor_of(layout, layout_node, first_letter) {
                return true;
            }
        }
        // The node's parent, or the host of the shadow root it is a child of: the ancestry a
        // `::first-letter` owner is looked for along.
        ancestor = engine
            .tree()
            .parent(current)
            .map(|parent| engine.tree().host_of(parent).unwrap_or(parent));
    }
    false
}

fn may_reuse_layout_node_for_child_list_insertion(
    layout: &TreeBuilderHost<'_>,
    engine: &StyleEngine,
    kind: PrincipalNodeKind,
    element: StyleNodeID,
) -> bool {
    let arena = layout.arena();
    let layout_node = arena.bound_row(element);
    if !kind.is_element()
        || layout_node.is_invalid()
        || engine.tree().shadow_root_of(element).is_some()
        || engine.element_is_slot(element)
        || engine.element_counter_reset_has_reversed_counter(element)
    {
        return false;
    }

    let last_layout_child = layout.last_child(layout_node);
    let trailing_inline_wrapper = if !last_layout_child.is_invalid()
        && node_facts::has_flag(layout.data(last_layout_child), NodeFlag::Anonymous)
        && node_facts::has_flag(layout.data(last_layout_child), NodeFlag::ChildrenAreInline)
        && !node_is_generated_for_pseudo_element(layout.data(last_layout_child))
    {
        last_layout_child
    } else {
        NodeSlotId::INVALID
    };

    let mut test = ChildListInsertionReuse {
        layout,
        engine,
        element,
        layout_node,
        has_first_letter_owner: false,
        trailing_inline_wrapper,
    };
    test.has_first_letter_owner = first_letter_owner_covers_subtree(layout, engine, element, layout_node);
    test.run()
}

impl ChildListInsertionReuse<'_, '_> {
    fn arena(&self) -> &LayoutNodeArena {
        self.layout.arena()
    }

    fn box_of(&self, node: StyleNodeID) -> LayoutNode {
        self.arena().bound_row(node)
    }

    fn published_display(&self, node: StyleNodeID) -> Option<FfiDisplay> {
        self.engine.element_published_box_facts(node).map(|facts| facts.display)
    }

    fn needs_layout_tree_update(&self, node: StyleNodeID) -> bool {
        self.arena().needs_layout_tree_update(node)
    }

    fn is_out_of_flow(&self, node: LayoutNode) -> bool {
        let data = self.layout.data(node);
        node_kind_is_node_with_style(data.kind.get()) && node_is_out_of_flow(self.layout, node)
    }

    fn parent_collapses_whitespace(&self) -> bool {
        self.layout
            .style(self.layout_node)
            .is_some_and(|style| style.white_space_collapse() == white_space_collapse::COLLAPSE)
    }

    /// Whether a text node holding only collapsing whitespace can be spliced in where it sits,
    /// which needs the boxes on both sides of it to be children of this box.
    fn collapsing_whitespace_can_be_inserted(&self, text: StyleNodeID) -> bool {
        let mut will_join_trailing_inline_wrapper = false;
        if !self.can_place_next_to_sibling(
            self.engine.tree().previous_sibling_in_dom_order(text),
            SiblingDirection::Previous,
            &mut will_join_trailing_inline_wrapper,
        ) || !self.can_place_next_to_sibling(
            self.engine.tree().next_sibling_in_dom_order(text),
            SiblingDirection::Next,
            &mut will_join_trailing_inline_wrapper,
        ) {
            return false;
        }

        // Incremental inline insertion always reuses a trailing anonymous inline wrapper. If the
        // whitespace belongs to a different run, only a full rebuild can place it correctly.
        self.trailing_inline_wrapper.is_invalid() || will_join_trailing_inline_wrapper
    }

    fn can_place_next_to_layout_node(
        &self,
        sibling_layout_node: LayoutNode,
        direction: SiblingDirection,
        pseudo_element: Option<u8>,
        will_join_trailing_inline_wrapper: &mut bool,
    ) -> bool {
        if sibling_layout_node.is_invalid() {
            return true;
        }
        if self.is_out_of_flow(sibling_layout_node) && (pseudo_element.is_some() || direction == SiblingDirection::Next)
        {
            return false;
        }

        let mut sibling_layout_node = sibling_layout_node;
        loop {
            let parent = self.layout.parent(sibling_layout_node);
            if parent.is_invalid() || parent == self.layout_node {
                break;
            }
            sibling_layout_node = parent;
        }
        if self.layout.parent(sibling_layout_node) != self.layout_node {
            return false;
        }
        let children_are_inline = node_facts::has_flag(self.layout.data(self.layout_node), NodeFlag::ChildrenAreInline);
        if let Some(generated_for) = pseudo_element {
            // ::after cannot anchor a newly appended anonymous wrapper. In normal flow,
            // inline ::before content also has to remain in its existing inline run, while
            // blockified flex/grid pseudo-elements remain separate items.
            if direction == SiblingDirection::Next {
                return children_are_inline;
            }

            let pseudo_display = self
                .engine
                .pseudo_published_box_facts(self.element, pseudo_kind_of(generated_for))
                .map(|facts| facts.display);
            let parent_display = self.layout.display(self.layout_node);
            let pseudo_belongs_to_inline_run = pseudo_display
                .is_some_and(|display| display.is_inline_outside() || display.is_contents())
                && !parent_display.is_flex_inside()
                && !parent_display.is_grid_inside();
            return children_are_inline || !pseudo_belongs_to_inline_run;
        }
        if node_facts::has_flag(self.layout.data(sibling_layout_node), NodeFlag::Anonymous) {
            if sibling_layout_node != self.trailing_inline_wrapper {
                return false;
            }
            *will_join_trailing_inline_wrapper = true;
        }
        true
    }

    fn can_place_next_to_sibling(
        &self,
        sibling: Option<StyleNodeID>,
        direction: SiblingDirection,
        will_join_trailing_inline_wrapper: &mut bool,
    ) -> bool {
        let mut sibling = sibling;
        while let Some(current) = sibling {
            if self
                .published_display(current)
                .is_some_and(|display| display.is_contents())
            {
                return false;
            }
            let sibling_layout_node = self.box_of(current);
            if !sibling_layout_node.is_invalid() {
                return self.can_place_next_to_layout_node(
                    sibling_layout_node,
                    direction,
                    None,
                    will_join_trailing_inline_wrapper,
                );
            }
            sibling = match direction {
                SiblingDirection::Next => self.engine.tree().next_sibling_in_dom_order(current),
                SiblingDirection::Previous => self.engine.tree().previous_sibling_in_dom_order(current),
            };
        }

        let generated_for = match direction {
            SiblingDirection::Previous => GENERATED_FOR_BEFORE,
            SiblingDirection::Next => GENERATED_FOR_AFTER,
        };
        self.can_place_next_to_layout_node(
            self.arena().bound_pseudo_element_row(self.element, generated_for),
            direction,
            Some(generated_for),
            will_join_trailing_inline_wrapper,
        )
    }

    fn dom_children(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        let mut next = self.engine.tree().dom_children(self.element).next();
        std::iter::from_fn(move || {
            let current = next?;
            next = self.engine.tree().next_sibling_in_dom_order(current);
            Some(current)
        })
    }

    /// Whether every child that has no box yet can be spliced in without the parent's own box
    /// being rebuilt: a collapsing whitespace text node, or an element that renders nothing.
    fn pending_children_can_preserve_parent(&self) -> bool {
        let collapses_whitespace = self.parent_collapses_whitespace();
        let mut has_pending_collapsing_whitespace = false;
        for child in self.dom_children() {
            if !self.box_of(child).is_invalid() {
                has_pending_collapsing_whitespace = false;
                if self.needs_layout_tree_update(child) || self.arena().child_needs_layout_tree_update(Some(child)) {
                    return false;
                }
                continue;
            }
            if !self.needs_layout_tree_update(child) {
                continue;
            }
            if child.text_index().is_some()
                && self.engine.tree().text_is_ascii_whitespace(child)
                && collapses_whitespace
                && !self.has_first_letter_owner
                && self.collapsing_whitespace_can_be_inserted(child)
            {
                if has_pending_collapsing_whitespace {
                    return false;
                }
                has_pending_collapsing_whitespace = true;
                continue;
            }
            if child.text_index().is_some() {
                return false;
            }
            if !self.published_display(child).is_some_and(|display| display.is_none()) {
                return false;
            }
        }
        true
    }

    /// Whether a row inserted between two existing rows would make the table fixup wrap whitespace
    /// that sits at the edge of a row group today.
    fn has_table_row_sibling(&self, sibling: Option<StyleNodeID>, direction: SiblingDirection) -> bool {
        let mut sibling = sibling;
        while let Some(current) = sibling {
            let sibling_layout_node = self.box_of(current);
            if !sibling_layout_node.is_invalid() {
                return self.layout.parent(sibling_layout_node) == self.layout_node
                    && node_kind_is_node_with_style(self.layout.data(sibling_layout_node).kind.get())
                    && self.layout.display(sibling_layout_node).is_table_row();
            }

            if !current.text_index().is_some() {
                let Some(sibling_display) = self.published_display(current) else {
                    return false;
                };
                if !sibling_display.is_none() {
                    return self.needs_layout_tree_update(current) && sibling_display.is_table_row();
                }
            } else if !self.engine.tree().text_is_ascii_whitespace(current) {
                return false;
            }
            sibling = match direction {
                SiblingDirection::Next => self.engine.tree().next_sibling_in_dom_order(current),
                SiblingDirection::Previous => self.engine.tree().previous_sibling_in_dom_order(current),
            };
        }
        false
    }

    fn run(&self) -> bool {
        if self.pending_children_can_preserve_parent() {
            // An empty set means every insertion was canceled before layout. Moves are marked dirty
            // at their destination, so they still enter one of the rejection paths above.
            return true;
        }

        let parent_display = self.layout.display(self.layout_node);
        let parent_has_children = !self.layout.first_child(self.layout_node).is_invalid();
        let parent_children_are_inline =
            node_facts::has_flag(self.layout.data(self.layout_node), NodeFlag::ChildrenAreInline);
        let parent_lays_out_flex_or_grid_children = parent_display.is_flex_inside() || parent_display.is_grid_inside();
        // A table cell lays out its contents as a flow root does.
        let parent_lays_out_flow =
            parent_display.is_flow_inside() || parent_display.is_flow_root_inside() || parent_display.is_table_cell();
        let parent_lays_out_inline_children =
            parent_lays_out_flow && (parent_children_are_inline || !parent_has_children);
        let parent_lays_out_block_children = parent_lays_out_flow && !parent_children_are_inline;
        let parent_lays_out_table_rows = parent_display.is_table_row_group()
            || parent_display.is_table_header_group()
            || parent_display.is_table_footer_group();
        if !parent_lays_out_flex_or_grid_children
            && !parent_lays_out_inline_children
            && !parent_lays_out_block_children
            && !parent_lays_out_table_rows
        {
            return false;
        }
        if self.has_first_letter_owner {
            return false;
        }

        if parent_lays_out_table_rows {
            // NB: Table fixup discards whitespace at the edge of a row group. Inserting a row outside
            //     that whitespace makes it interior, so only a rebuild can create its anonymous table-row box.
            for child in self.dom_children() {
                if !child.text_index().is_some()
                    || !self.box_of(child).is_invalid()
                    || self.needs_layout_tree_update(child)
                    || !self.engine.tree().text_is_ascii_whitespace(child)
                {
                    continue;
                }
                if self.has_table_row_sibling(
                    self.engine.tree().previous_sibling_in_dom_order(child),
                    SiblingDirection::Previous,
                ) && self.has_table_row_sibling(
                    self.engine.tree().next_sibling_in_dom_order(child),
                    SiblingDirection::Next,
                ) {
                    return false;
                }
            }
        }

        let collapses_whitespace = self.parent_collapses_whitespace();
        let mut will_insert_inline_child = false;
        let mut will_insert_block_child = false;
        let mut all_inserted_block_children_are_in_flow = true;
        let mut has_indirect_existing_child = false;
        let mut has_indirect_existing_child_after_insertion = false;
        let mut has_inserted_child = false;
        // A fieldset keeps its content in a structural anonymous content box that appended children must also enter.
        let mut all_indirect_existing_children_are_in_text_run_wrappers =
            self.layout.data(self.layout_node).kind.get() != NodeKind::FieldSetBox;
        let mut has_pending_collapsing_whitespace = false;
        for child in self.dom_children() {
            let child_layout_node = self.box_of(child);
            if !child_layout_node.is_invalid() {
                has_pending_collapsing_whitespace = false;
                if self.layout.parent(child_layout_node) != self.layout_node {
                    let wrapper = self.layout.parent(child_layout_node);
                    if wrapper.is_invalid()
                        || self.layout.parent(wrapper) != self.layout_node
                        || !node_facts::has_flag(self.layout.data(wrapper), NodeFlag::Anonymous)
                        || !node_facts::has_flag(self.layout.data(wrapper), NodeFlag::ChildrenAreInline)
                        || node_is_generated_for_pseudo_element(self.layout.data(wrapper))
                    {
                        all_indirect_existing_children_are_in_text_run_wrappers = false;
                    }
                    has_indirect_existing_child = true;
                    if has_inserted_child {
                        has_indirect_existing_child_after_insertion = true;
                    }
                }
                continue;
            }

            if child.text_index().is_some() {
                if !self.needs_layout_tree_update(child) {
                    continue;
                }
                let collapsed_whitespace_can_be_inserted = self.engine.tree().text_is_ascii_whitespace(child)
                    && collapses_whitespace
                    && !self.has_first_letter_owner
                    && self.collapsing_whitespace_can_be_inserted(child);
                if !collapsed_whitespace_can_be_inserted || has_pending_collapsing_whitespace {
                    return false;
                }
                has_pending_collapsing_whitespace = true;
                continue;
            }

            let Some(child_facts) = self.engine.element_published_box_facts(child) else {
                return false;
            };
            let child_display = child_facts.display;
            if child_display.is_contents() {
                return false;
            }
            if !self.needs_layout_tree_update(child) || child_display.is_none() {
                continue;
            }
            if self.engine.subtree_affects_generated_content_state(child) {
                return false;
            }
            let child_type_facts = self.engine.element_adjustment_facts(child);
            if child_type_facts
                & (element_adjustment_fact::RENDERED_IN_TOP_LAYER | element_adjustment_fact::IS_SVG_ELEMENT)
                != 0
            {
                return false;
            }
            let child_is_in_flow = child_facts.position != positioning::ABSOLUTE
                && child_facts.position != positioning::FIXED
                && child_facts.float_ == float::NONE;
            if parent_lays_out_flex_or_grid_children {
                if has_pending_collapsing_whitespace
                    && (child_facts.position != positioning::STATIC || child_facts.float_ != float::NONE)
                {
                    return false;
                }
                has_pending_collapsing_whitespace = false;
                has_inserted_child = true;
                continue;
            }
            if parent_lays_out_table_rows && child_display.is_table_row() {
                continue;
            }
            if parent_lays_out_block_children && child_display.is_block_outside() {
                if has_pending_collapsing_whitespace
                    && (child_facts.position != positioning::STATIC || child_facts.float_ != float::NONE)
                {
                    return false;
                }
                has_pending_collapsing_whitespace = false;
                has_inserted_child = true;
                will_insert_block_child = true;
                if !child_is_in_flow {
                    all_inserted_block_children_are_in_flow = false;
                }
                if will_insert_inline_child {
                    return false;
                }
                continue;
            }
            if parent_lays_out_inline_children
                && child_display.is_inline_outside()
                && (child_display.is_flow_root_inside()
                    || child_display.is_flex_inside()
                    || child_display.is_grid_inside())
            {
                will_insert_inline_child = true;
                has_inserted_child = true;
                if will_insert_block_child {
                    return false;
                }
                continue;
            }
            // An absolutely positioned box joins the inline formatting context as an item of its own,
            // without wrapping its inline siblings, whatever its outer display type.
            if parent_lays_out_inline_children
                && !will_insert_block_child
                && (child_facts.position == positioning::ABSOLUTE || child_facts.position == positioning::FIXED)
            {
                if has_pending_collapsing_whitespace {
                    return false;
                }
                will_insert_inline_child = true;
                has_inserted_child = true;
                continue;
            }
            return false;
        }
        // OPTIMIZATION: Appending an in-flow block after every existing child cannot disturb an
        //               earlier anonymous inline wrapper, and needs no indirect sibling anchor. A flex or grid
        //               container never places an appended item, in flow or out of flow, into an existing anonymous
        //               text run wrapper, so appending any items after every existing child is safe there too.
        let appends_independent_children =
            (parent_lays_out_block_children && will_insert_block_child && all_inserted_block_children_are_in_flow)
                || (parent_lays_out_flex_or_grid_children
                    && has_inserted_child
                    && all_indirect_existing_children_are_in_text_run_wrappers);
        let can_append_after_indirect_existing_children =
            appends_independent_children && !has_indirect_existing_child_after_insertion;
        (!has_indirect_existing_child || can_append_after_indirect_existing_children)
            && !has_pending_collapsing_whitespace
    }
}

pub(crate) fn principal_node_entry_decision(
    facts: PrincipalNodeEntryFacts,
    reuse: LayoutNodeReuse,
    kind: PrincipalNodeKind,
    element_type_facts: u32,
    context: &TreeBuilderContext,
) -> PrincipalNodeEntryDecision {
    let should_create_layout_node = facts.must_create_subtree
        || (facts.needs_layout_tree_update && !reuse.insert_children && !reuse.update_pseudo_elements)
        || context.document_needs_full_layout_tree_update
        || (kind.is_document() && !facts.has_layout_node);

    let has = |fact: u32| element_type_facts & fact != 0;
    let top_layer =
        if kind.is_element() && has(element_adjustment_fact::RENDERED_IN_TOP_LAYER) && !context.layout_top_layer {
            if !facts.layout_node_is_attached && !facts.needs_layout_tree_update {
                TopLayerEntryDecision::SkipAndRequestZoneRebuild
            } else {
                TopLayerEntryDecision::Skip
            }
        } else {
            TopLayerEntryDecision::Continue
        };

    let requires_svg_container = has(element_adjustment_fact::REQUIRES_SVG_CONTAINER);
    let svg = if has(element_adjustment_fact::IS_SVG_CONTAINER) {
        SvgEntryDecision::EnterSvgRoot
    } else if requires_svg_container && !context.has_svg_root {
        SvgEntryDecision::Skip
    } else if has(element_adjustment_fact::IS_SVG_FOREIGN_OBJECT_ELEMENT) {
        SvgEntryDecision::EnterForeignContent
    } else if kind.is_element() && !requires_svg_container && context.has_svg_root {
        SvgEntryDecision::Skip
    } else {
        SvgEntryDecision::Continue
    };

    PrincipalNodeEntryDecision {
        should_create_layout_node,
        top_layer,
        svg,
    }
}

impl TreeBuilderHost<'_> {
    /// The first node in the DOM child sequence the style mirror holds for `parent`.
    fn first_dom_child(&self, parent: StyleNodeID) -> Option<StyleNodeID> {
        self.arena().first_dom_child(parent)
    }

    /// The node after `node` in the DOM child sequence its parent holds.
    fn next_dom_sibling(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.arena().next_dom_sibling(node)
    }

    /// The element facts the style mirror holds for a node the walk reached.
    fn element_type_facts(&self, style_node: Option<StyleNodeID>) -> u32 {
        self.arena().element_adjustment_facts(style_node)
    }

    /// Whether the style mirror holds the element in the top layer.
    fn rendered_in_top_layer(&self, style_node: Option<StyleNodeID>) -> bool {
        self.element_type_facts(style_node) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0
    }

    /// The display the element's published style record asks for.
    fn published_display(&self, style_node: Option<StyleNodeID>) -> FfiDisplay {
        self.arena()
            .published_box_facts(style_node)
            .expect("an element the walk prepares has published its style")
            .display
    }

    /// Whether the element's published style record hides its content. Only an element has a
    /// record, so every other node answers no, as its `content-visibility` never applied.
    fn content_visibility_is_hidden(&self, style_node: Option<StyleNodeID>) -> bool {
        self.arena()
            .published_box_facts(style_node)
            .is_some_and(|facts| facts.content_visibility == crate::css::css_enums::content_visibility::HIDDEN)
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

/// Whether the node projects assigned nodes as a slot, and whether the walk lays out its own DOM
/// children. A slot lays out its children only as fallback content, when nothing is assigned to it.
fn dom_child_layout_plan(host: &TreeBuilderHost<'_>, node: StyleNodeID) -> (bool, bool) {
    let has_assigned_nodes = host.arena().assigned_node_count(Some(node)) != 0;
    (
        has_assigned_nodes,
        !has_assigned_nodes && host.first_dom_child(node).is_some(),
    )
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
        host.arena().set_document_style_node(document_identity);
        let mut state = TreeBuilderState::default();
        // The viewport's style is the document's, which the host makes rather than publishes, so a
        // build that may build the viewport is handed it before it starts.
        if let Some(record) = self.document_style_record {
            state.document_style = Some(
                host.arena()
                    .with_style_engine(|engine| DerivedStyleRecord::pin(engine, record)),
            );
        }
        let mut context = TreeBuilderContext {
            document_needs_full_layout_tree_update: host.arena().needs_full_layout_tree_update(),
            ..Default::default()
        };
        // Whether the document already had a viewport, read before the build replaces it.
        let document_had_layout_node = !host.arena().layout_root().is_invalid();

        update_layout_tree_from(
            host,
            &mut state,
            document_identity,
            &mut context,
            false,
            FfiInsertionMode::Append,
            true,
        );

        let document_layout_node = host.arena().layout_root();
        let rebuilt_subtrees_were_updated_individually = !document_layout_node.is_invalid()
            && !(context.document_needs_full_layout_tree_update
                || !document_had_layout_node
                || state.layout_tree_update_escaped_rebuild_roots);
        if !document_layout_node.is_invalid() {
            if rebuilt_subtrees_were_updated_individually {
                fixup_tables_in_rebuilt_subtrees(
                    host,
                    &state.rebuilt_subtree_roots,
                    &state.reused_child_list_update_roots,
                    &state.additional_table_fixup_roots,
                );
            } else {
                host.arena().set_needs_full_scrollable_overflow_recalculation();
                fixup_tables(host, document_layout_node);
            }

            // https://drafts.csswg.org/css-scrollbars/#scrollbar-width
            // UAs must apply the scrollbar-color value set on the root element to the viewport.
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
                    .misc_reset()
                    .scrollbar_width;
                host.arena()
                    .update_layout_style(host.host_calls(), document_layout_node, |style| {
                        style.set_scrollbar_width(scrollbar_width);
                    });
            }
        }

        for &element in &state.layout_tree_rebuild_requests {
            // A request that names no element asks for the whole tree, which the arena answers itself.
            let Some(element) = element else {
                host.arena().set_needs_full_layout_tree_update(true);
                continue;
            };
            state.reports.push(crate::layout::commit::FfiCommitMessage::new(
                element.raw(),
                crate::layout::commit::FfiCommitMessageKind::LayoutTreeRebuildRequested,
            ));
        }

        let built_scroll_containers = host.arena().take_built_scroll_containers();
        let arena = host.arena();
        if rebuilt_subtrees_were_updated_individually {
            let attached_roots = arena.derive_facts_after_tree_update(&state.rebuilt_subtree_roots);
            arena.resolve_deferred_child_list_insertions(&attached_roots);
        } else {
            // NB: The full layout entry must derive the facts of this tree.
            arena.record_partial_relayout_escape();
            arena.resolve_deferred_child_list_insertions(&Default::default());
        }

        // Table fixup can free a rebuilt root after it was recorded, such as whitespace at the edge of a
        // row group, so only the roots that are still live wait for the partial relayout plan.
        let live_rebuilt_subtree_roots: Vec<NodeSlotId> = state
            .rebuilt_subtree_roots
            .iter()
            .copied()
            .filter(|root| arena.slot_is_live(*root))
            .collect();
        let rebuilt_subtree_root_count = live_rebuilt_subtree_roots.len();
        arena.set_pending_rebuilt_subtree_roots(
            live_rebuilt_subtree_roots,
            state.layout_tree_update_escaped_rebuild_roots,
        );
        let viewport = arena.layout_root();
        assert!(!viewport.is_invalid(), "a layout tree build places the viewport");
        state.release_pinned_style_records(arena);
        let outcome = FfiLayoutTreeBuildOutcome {
            viewport,
            rebuilt_subtree_root_count,
            layout_tree_update_escaped_rebuild_roots: state.layout_tree_update_escaped_rebuild_roots,
            needs_another_build_pass: !state.layout_tree_rebuild_requests.is_empty() || state.reached_unstyled_element,
        };
        TreeBuildAnswer {
            outcome,
            reports: state.reports,
            built_scroll_containers,
        }
    }
}

/// Updates every direct DOM child in tree order.
fn update_layout_tree_for_dom_children(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    parent: StyleNodeID,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
    insertion_mode: FfiInsertionMode,
) {
    let mut node = host.first_dom_child(parent);
    while let Some(current) = node {
        update_layout_tree(host, state, current, context, must_create_subtree, insertion_mode);
        node = host.next_dom_sibling(current);
    }
}

/// Updates every shadow-root child in tree order and clears the root's update flags.
fn update_layout_tree_for_shadow_root_children(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    shadow_root: StyleNodeID,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
) {
    update_layout_tree_for_dom_children(
        host,
        state,
        shadow_root,
        context,
        must_create_subtree,
        FfiInsertionMode::Append,
    );
    host.arena().clear_layout_tree_update_marks(Some(shadow_root));
}

/// Updates a slot's assigned nodes in flat-tree order.
fn update_layout_tree_for_assigned_slottables(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    slot: StyleNodeID,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
) {
    // The style mirror holds the assigned nodes in flat-tree order, and the list does not change while the build
    // walks it.
    for index in 0..host.arena().assigned_node_count(Some(slot)) {
        let node = host.arena().assigned_node_at(slot, index);
        update_layout_tree(
            host,
            state,
            node,
            context,
            must_create_subtree,
            FfiInsertionMode::Append,
        );
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

/// Detaches the top layer element `element`'s layout placement and clears every stale projected
/// subtree of it. Called at DOM mutation processing time, outside layout tree construction.
pub(crate) fn detach_top_layer_element_layout_subtree(
    host_calls: HostCalls<'_>,
    arena: &mut LayoutNodeArena,
    element: StyleNodeID,
) {
    let element_layout_node = arena.bound_row(element);
    if !element_layout_node.is_invalid() {
        let topmost = topmost_layout_node_of_top_layer_placement(arena, element_layout_node);
        let layout_node_to_detach = if topmost.is_invalid() {
            element_layout_node
        } else {
            topmost
        };
        super::layout_node_arena::prepare_subtree_for_detach(host_calls, arena, layout_node_to_detach);
        if arena.detach_from_parent(layout_node_to_detach) {
            host_calls.free_subtree(arena, layout_node_to_detach);
        }
    }

    let host = &mut TreeBuilderHost {
        arena,
        work: host_calls.0,
    };
    clear_stale_subtree(host, element, StaleSubtreeClearScope::InclusiveBoundedToRoot);
    clear_stale_assigned_slottables(host, element);
}

/// Every pseudo-element of the element gives up the box it holds, subtree and all.
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
        clear_synthetic_pseudo_element_boxes(host_calls, arena, node);
    }
    let row = arena.bound_row(node);
    if row.is_invalid() {
        return;
    }
    arena.pin_style_record_for_detachment(row);
    crate::painting::ffi::paintable_cleared_from_node(host_calls, arena, row);
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
    for generated_for in GENERATED_FOR_AFTER..=crate::layout::node_data::GENERATED_FOR_LAST_SYNTHETIC {
        free_pseudo_element_box(host_calls, arena, node, generated_for);
    }
}

/// The pseudo-element of kind `generated_for` on the element `node` gives up its box, subtree and
/// all. Answers whether the box was attached under a parent, or none when there was no box.
fn free_pseudo_element_box(
    host_calls: HostCalls<'_>,
    arena: &mut LayoutNodeArena,
    node: StyleNodeID,
    generated_for: u8,
) -> Option<bool> {
    let row = arena.bound_pseudo_element_row(node, generated_for);
    if row.is_invalid() {
        return None;
    }
    let mut rows = Vec::new();
    arena.for_each_node_in_layout_subtree_in_pre_order(row, |row| rows.push(row));
    for row in rows {
        crate::painting::ffi::paintable_cleared_from_node(host_calls, arena, row);
    }
    super::layout_node_arena::prepare_subtree_for_detach(host_calls, arena, row);
    let was_attached = arena.detach_from_parent(row);
    host_calls.free_subtree(arena, row);
    Some(was_attached)
}

/// Clears every stale layout node in the shadow-including subtree `root` names.
///
/// A DOM walk visits a node, then its shadow root's subtree, then its DOM children. A node the
/// style mirror has not named holds no layout tree update mark and can have no box, so navigating
/// the mirror's DOM child sequence reaches everything such a walk had work for, in the same order.
fn clear_stale_subtree(host: &mut TreeBuilderHost, root: StyleNodeID, scope: StaleSubtreeClearScope) {
    let cleared_subtree_root = (scope != StaleSubtreeClearScope::Inclusive).then_some(root);
    let facts = host.arena().stale_walk_facts(root);
    if scope == StaleSubtreeClearScope::DescendantsBoundedToRoot {
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

/// Removes the stale layout subtree of every node a slot projects, for a slot whose own box hides
/// its content.
fn clear_stale_assigned_slottables(host: &mut TreeBuilderHost, slot: StyleNodeID) {
    for index in 0..host.arena().assigned_node_count(Some(slot)) {
        let node = host.arena().assigned_node_at(slot, index);
        clear_stale_subtree(host, node, StaleSubtreeClearScope::InclusiveBoundedToRoot);
    }
}

/// Applies SVG `<switch>` child selection and updates its rendered child.
fn update_layout_tree_for_svg_switch_children(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    switch_element: StyleNodeID,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
) {
    // https://svgwg.org/svg2-draft/struct.html#SwitchElement
    // The ‘switch’ element evaluates the ‘requiredExtensions’ and ‘systemLanguage’ attributes on its direct child
    // elements in order, and then processes and renders the first child for which these attributes evaluate to
    // true. All others will be bypassed and therefore not rendered. If the child element is a container element
    // such as a ‘g’, then the entire subtree is either processed/rendered or bypassed/not rendered.
    let mut rendered_child = None;
    let mut child = host.first_dom_child(switch_element);
    while let Some(current) = child {
        // FIXME: Evaluate the requiredExtensions and systemLanguage attributes.
        if host.element_type_facts(Some(current)) & element_adjustment_fact::IS_SVG_ELEMENT != 0 {
            rendered_child = Some(current);
            break;
        }
        child = host.next_dom_sibling(current);
    }

    // NB: Clean up any stale children that should no longer be rendered.
    let mut child = host.first_dom_child(switch_element);
    while let Some(current) = child {
        if child != rendered_child {
            host.clear_stale_layout_node(current, None);
        }
        child = host.next_dom_sibling(current);
    }

    if let Some(rendered_child) = rendered_child {
        update_layout_tree(
            host,
            state,
            rendered_child,
            context,
            must_create_subtree,
            FfiInsertionMode::Append,
        );
    }
}

/// Updates an element that generates no principal box because it has `display: contents`.
fn update_layout_tree_for_display_contents(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    style_node: StyleNodeID,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
    should_create_layout_node: bool,
) {
    let (has_assigned_nodes, lays_out_dom_children) = dom_child_layout_plan(host, style_node);
    let content_visibility_hidden = host.content_visibility_is_hidden(Some(style_node));

    // A display:contents member builds its children through this path, so the top layer flag
    // is consumed here the same way update_layout_tree does for members with a box.
    let clear_layout_top_layer_for_descendants =
        host.rendered_in_top_layer(Some(style_node)) && context.layout_top_layer;
    if clear_layout_top_layer_for_descendants {
        context.layout_top_layer = false;
    }

    if should_create_layout_node {
        clear_stale_subtree(host, style_node, StaleSubtreeClearScope::Inclusive);
        resolve_counters(host, style_node, FfiPseudoElement::None);
    }

    if should_create_layout_node && !content_visibility_hidden && !context.has_svg_root {
        let placed = create_pseudo_element(
            host,
            state,
            style_node,
            FfiPseudoElement::Before,
            Some(FfiInsertionMode::Append),
        );
        assert!(placed.is_none());
    }

    let child_needs_layout_tree_update = host.arena().child_needs_layout_tree_update(Some(style_node));
    if !content_visibility_hidden && (should_create_layout_node || child_needs_layout_tree_update) {
        let must_create_children = should_create_layout_node;
        if let Some(shadow_root) = host.arena().shadow_root_of(style_node) {
            update_layout_tree_for_shadow_root_children(host, state, shadow_root, context, must_create_children);
        } else if lays_out_dom_children {
            update_layout_tree_for_dom_children(
                host,
                state,
                style_node,
                context,
                must_create_children,
                FfiInsertionMode::Append,
            );
        }
    }

    if has_assigned_nodes {
        if !content_visibility_hidden {
            update_layout_tree_for_assigned_slottables(
                host,
                state,
                style_node,
                context,
                must_create_subtree || should_create_layout_node,
            );
        } else {
            clear_stale_assigned_slottables(host, style_node);
        }
    }

    if should_create_layout_node && !content_visibility_hidden && !context.has_svg_root {
        let placed = create_pseudo_element(
            host,
            state,
            style_node,
            FfiPseudoElement::After,
            Some(FfiInsertionMode::Append),
        );
        assert!(placed.is_none());
    }

    host.arena().clear_layout_tree_update_marks(Some(style_node));

    if clear_layout_top_layer_for_descendants {
        context.layout_top_layer = true;
    }
}

fn ancestor_stack_contains_element_box(arena: &LayoutNodeArena, state: &TreeBuilderState, style_node: u32) -> bool {
    // An element's box binds itself to the element at construction time, before its subtree is
    // built, so an element under construction anywhere on the ancestor stack is found here.
    let element_box = bound_box(arena, style_node);
    !element_box.is_invalid() && state.ancestor_stack.contains(&element_box)
}

/// Tells the document that a graphics element's box now holds the content of an SVG resource. The
/// resource outlives that box, so the document has to rebuild the referencing subtree when the
/// resource goes away or changes, which is the only thing it does with this.
fn report_svg_resource_reference(state: &mut TreeBuilderState, resource: u32, graphics_element: u32) {
    state.reports.push(crate::layout::commit::FfiCommitMessage {
        style_node: resource,
        other_style_node: graphics_element,
        kind: crate::layout::commit::FfiCommitMessageKind::SvgResourceReferenced,
    });
}

fn update_svg_resource(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    resource: StyleNodeID,
    graphics_element: StyleNodeID,
    layout_node: LayoutNode,
    context: &mut TreeBuilderContext,
    prior_context_value: bool,
) {
    context.layout_svg_mask_or_clip_path = true;
    let prior_has_svg_root = context.has_svg_root;
    context.has_svg_root = true;
    state.ancestor_stack.push(layout_node);

    if !ancestor_stack_contains_element_box(host.arena(), state, resource.raw()) {
        update_layout_tree(host, state, resource, context, true, FfiInsertionMode::Append);
        report_svg_resource_reference(state, resource.raw(), graphics_element.raw());
    } else {
        // FIXME: Somehow either remove ancestor from the layout tree or mark it as invalid.
    }

    assert!(state.ancestor_stack.pop().is_some());
    context.has_svg_root = prior_has_svg_root;
    context.layout_svg_mask_or_clip_path = prior_context_value;
}

/// The pattern whose children a `<pattern>` draws: itself if it has element children, and
/// otherwise the pattern its `href` chain leads to, following
/// `SVGPatternElement::pattern_content_element`.
///
/// The chain is walked here rather than asked of the document: a pattern publishes its `href`'s
/// fragment as an id atom, and the mirror's id index answers what that atom names. Only the
/// document scope is searched, which is where `SVGPatternElement::linked_pattern` searches.
fn svg_pattern_content_element(host: &TreeBuilderHost<'_>, pattern: StyleNodeID) -> Option<StyleNodeID> {
    let arena = host.arena();
    let mut current = pattern;
    // A pattern may name itself somewhere along the chain, so every pattern stepped to is
    // remembered and a second arrival ends the walk. A chain is at most a handful of links long.
    let mut seen = Vec::new();
    loop {
        if arena.has_dom_element_children(current) {
            return Some(current);
        }
        let atom = arena.style_node_svg_attribute_facts(current).reference_fragment_atom;
        let linked = arena.element_by_document_id(atom)?;
        if linked == current || seen.contains(&linked) {
            return None;
        }
        if host.element_type_facts(Some(linked)) & element_adjustment_fact::IS_SVG_PATTERN_ELEMENT == 0 {
            return None;
        }
        seen.push(linked);
        current = linked;
    }
}

/// The element one of `mask`, `clip-path`, `fill` and `stroke` names: the id index resolved in
/// the referrer's scope order, and then the element type the resource requires. A reference to an
/// element of any other type names nothing at all.
fn svg_style_reference_element(
    host: &TreeBuilderHost<'_>,
    referrer: StyleNodeID,
    atom: u32,
    required_element_fact: u32,
) -> Option<StyleNodeID> {
    let resolved = host.arena().element_by_svg_reference(referrer, atom)?;
    (host.element_type_facts(Some(resolved)) & required_element_fact != 0).then_some(resolved)
}

fn update_svg_pattern(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    pattern: StyleNodeID,
    content_element: StyleNodeID,
    graphics_element: StyleNodeID,
    layout_node: LayoutNode,
    context: &mut TreeBuilderContext,
) {
    let prior_context_value = context.layout_svg_pattern;
    context.layout_svg_pattern = true;
    state.ancestor_stack.push(layout_node);

    if !ancestor_stack_contains_element_box(host.arena(), state, content_element.raw()) {
        update_layout_tree(host, state, content_element, context, true, FfiInsertionMode::Append);
        // The referenced pattern may inherit its content from another pattern via href. Removing either element
        // invalidates the attached resource box, so register the referencer with both.
        report_svg_resource_reference(state, content_element.raw(), graphics_element.raw());
        if pattern != content_element {
            report_svg_resource_reference(state, pattern.raw(), graphics_element.raw());
        }
    }

    assert!(state.ancestor_stack.pop().is_some());
    context.layout_svg_pattern = prior_context_value;
}

struct PrincipalDescendantUpdate {
    kind: PrincipalNodeKind,
    style_node: Option<StyleNodeID>,
    /// The node's identity in the style mirror: its style node, or the document's own for the
    /// document. It owns the DOM child sequence the walk descends into and the node's layout tree
    /// update marks.
    mirror_identity: StyleNodeID,
    element_type_facts: u32,
    should_create_layout_node: bool,
    update_pseudo_elements_in_place: bool,
    must_create_subtree: bool,
    insertion_mode: FfiInsertionMode,
}

/// Updates the descendants and post-child state of a node with a principal layout box.
fn update_principal_node_descendants(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    layout_node: LayoutNode,
    context: &mut TreeBuilderContext,
    update: PrincipalDescendantUpdate,
) {
    let should_create_layout_node = update.should_create_layout_node;
    assert!(!layout_node.is_invalid());
    let (has_assigned_nodes, lays_out_dom_children) = dom_child_layout_plan(host, update.mirror_identity);
    let shadow_root = if update.kind.is_element() {
        host.arena().shadow_root_of(update.mirror_identity)
    } else {
        None
    };
    let content_visibility_hidden = host.content_visibility_is_hidden(update.style_node);
    let (layout_node_can_have_children, layout_node_is_replaced_box_with_children) = {
        let layout_node_data = host.data(layout_node);
        let can_have_children = node_facts::node_can_have_children(layout_node_data);
        (
            can_have_children,
            node_facts::kind_is_replaced_box(layout_node_data.kind.get()) && can_have_children,
        )
    };
    let prior_quote_nesting_level = state.quote_nesting_level;

    if should_create_layout_node || update.update_pseudo_elements_in_place {
        // Resolve counters now that we exist in the layout tree.
        if should_create_layout_node && update.kind.is_element() {
            resolve_counters(host, update.mirror_identity, FfiPseudoElement::None);
        }

        // Add the ::before pseudo-element before walking normal children.
        if update.kind.is_element()
            && layout_node_can_have_children
            && !content_visibility_hidden
            && !context.has_svg_root
        {
            state.ancestor_stack.push(layout_node);
            let placed = create_pseudo_element(
                host,
                state,
                update.mirror_identity,
                FfiPseudoElement::Before,
                Some(FfiInsertionMode::Prepend),
            );
            assert!(placed.is_none());
            assert!(state.ancestor_stack.pop().is_some());
        }
    }

    if content_visibility_hidden {
        clear_stale_subtree(
            host,
            update.mirror_identity,
            StaleSubtreeClearScope::DescendantsBoundedToRoot,
        );
    }

    if (should_create_layout_node
        || host
            .arena()
            .child_needs_layout_tree_update(Some(update.mirror_identity)))
        && (shadow_root.is_some() || lays_out_dom_children)
        && layout_node_can_have_children
        && !content_visibility_hidden
    {
        state.ancestor_stack.push(layout_node);

        if let Some(shadow_root) = shadow_root {
            if layout_node_is_replaced_box_with_children {
                // For replaced elements with shadow DOM children, wrap the children in an
                // anonymous BlockContainer so that a BFC handles their layout.
                let first_child = host.first_child(layout_node);
                if first_child.is_invalid() || !node_facts::has_flag(host.data(first_child), NodeFlag::Anonymous) {
                    let wrapper = host.create_anonymous_wrapper_box(layout_node);
                    host.attach_child(state.current_parent(), wrapper, NodeSlotId::INVALID);
                }
                let wrapper = host.first_child(layout_node);
                assert!(!wrapper.is_invalid());
                state.ancestor_stack.push(wrapper);
            }
            update_layout_tree_for_shadow_root_children(host, state, shadow_root, context, should_create_layout_node);
            if layout_node_is_replaced_box_with_children {
                assert!(state.ancestor_stack.pop().is_some());
            }
        } else if lays_out_dom_children {
            if update.element_type_facts & element_adjustment_fact::IS_SVG_SWITCH_ELEMENT != 0 {
                update_layout_tree_for_svg_switch_children(
                    host,
                    state,
                    update.mirror_identity,
                    context,
                    should_create_layout_node,
                );
            } else {
                update_layout_tree_for_dom_children(
                    host,
                    state,
                    update.mirror_identity,
                    context,
                    should_create_layout_node,
                    update.insertion_mode,
                );
            }
        }

        if update.kind.is_document() {
            // Elements in the top layer do not lay out normally based on their position in the document; instead
            // they generate boxes as if they were siblings of the root element.
            let prior_layout_top_layer = context.layout_top_layer;
            context.layout_top_layer = true;
            // The walk below reads the store again, so the list is read a member at a time rather
            // than borrowed across it.
            for index in 0.. {
                let Some(member) = host.arena().top_layer_element(index) else {
                    break;
                };
                if !host.rendered_in_top_layer(Some(member)) {
                    continue;
                }
                if has_unrendered_flat_tree_ancestor(host, Some(member)) {
                    clear_stale_subtree(host, member, StaleSubtreeClearScope::InclusiveBoundedToRoot);
                    continue;
                }
                update_layout_tree(
                    host,
                    state,
                    member,
                    context,
                    should_create_layout_node,
                    FfiInsertionMode::Append,
                );
            }
            context.layout_top_layer = prior_layout_top_layer;
        }

        assert!(state.ancestor_stack.pop().is_some());
    }

    if has_assigned_nodes {
        if !content_visibility_hidden {
            state.ancestor_stack.push(layout_node);
            update_layout_tree_for_assigned_slottables(
                host,
                state,
                update.mirror_identity,
                context,
                update.must_create_subtree || should_create_layout_node,
            );
            assert!(state.ancestor_stack.pop().is_some());
        } else {
            clear_stale_assigned_slottables(host, update.mirror_identity);
        }
    }

    if should_create_layout_node {
        let svg_attributes = update
            .style_node
            .map(|referrer| (referrer, host.arena().style_node_svg_attribute_facts(referrer)))
            .filter(|(_, facts)| facts.is_graphics_element);
        if let Some((referrer, svg_attributes)) = svg_attributes {
            // The references name elements of the style mirror, which updating a resource's box leaves alone.
            let reference = |atom, required| svg_style_reference_element(host, referrer, atom, required);
            let masks = [
                reference(
                    svg_attributes.mask_reference_atom,
                    element_adjustment_fact::IS_SVG_MASK_ELEMENT,
                ),
                reference(
                    svg_attributes.clip_path_reference_atom,
                    element_adjustment_fact::IS_SVG_CLIP_PATH_ELEMENT,
                ),
            ];
            let patterns = [
                reference(
                    svg_attributes.fill_reference_atom,
                    element_adjustment_fact::IS_SVG_PATTERN_ELEMENT,
                ),
                reference(
                    svg_attributes.stroke_reference_atom,
                    element_adjustment_fact::IS_SVG_PATTERN_ELEMENT,
                ),
            ];
            for resource in masks.into_iter().flatten() {
                update_svg_resource(
                    host,
                    state,
                    resource,
                    referrer,
                    layout_node,
                    context,
                    context.layout_svg_mask_or_clip_path,
                );
            }

            let mut seen_content_elements = Vec::with_capacity(2);
            for pattern in patterns.into_iter().flatten() {
                let Some(content_element) = svg_pattern_content_element(host, pattern) else {
                    continue;
                };
                if seen_content_elements.contains(&content_element) {
                    continue;
                }
                seen_content_elements.push(content_element);
                update_svg_pattern(host, state, pattern, content_element, referrer, layout_node, context);
            }
        }

        // Add ::marker and ::after once normal and SVG resource children are complete.
        if update.kind.is_element()
            && layout_node_can_have_children
            && !content_visibility_hidden
            && !context.has_svg_root
        {
            state.ancestor_stack.push(layout_node);
            if host.data(layout_node).kind.get() == NodeKind::ListItemBox {
                let placed = create_pseudo_element(
                    host,
                    state,
                    update.mirror_identity,
                    FfiPseudoElement::Marker,
                    Some(FfiInsertionMode::Prepend),
                );
                assert!(placed.is_none());
            }
            let placed = create_pseudo_element(
                host,
                state,
                update.mirror_identity,
                FfiPseudoElement::After,
                Some(FfiInsertionMode::Append),
            );
            assert!(placed.is_none());
            assert!(state.ancestor_stack.pop().is_some());

            if node_facts::kind_is_block_container(host.data(layout_node).kind.get())
                && host.has_first_letter_style(layout_node)
            {
                let target = find_first_letter_in_block(host, layout_node);
                if target.found {
                    create_first_letter_boxes(host, update.mirror_identity, target);
                }
            }
        }

        wrap_fieldset_contents_if_needed(host, layout_node);
        wrap_button_contents_if_needed(host, layout_node);
    }

    if update.update_pseudo_elements_in_place && !should_create_layout_node {
        state.ancestor_stack.push(layout_node);
        let placed = create_pseudo_element(
            host,
            state,
            update.mirror_identity,
            FfiPseudoElement::After,
            Some(FfiInsertionMode::Append),
        );
        assert!(placed.is_none());
        assert!(state.ancestor_stack.pop().is_some());
    }

    // https://www.w3.org/TR/css-contain-2/#containment-style
    // Giving an element style containment has the following effects:
    // 2. The effects of the 'content' property’s 'open-quote', 'close-quote', 'no-open-quote' and 'no-close-quote'
    //    must be scoped to the element’s sub-tree.
    if node_facts::node_style_view(host.data(layout_node))
        .is_some_and(crate::painting::style_queries::has_style_containment)
    {
        state.quote_nesting_level = prior_quote_nesting_level;
    }

    host.arena()
        .clear_layout_tree_update_marks(Some(update.mirror_identity));
}

struct PrincipalNodeUpdate<'host, 'callbacks, 'state, 'context> {
    kind: PrincipalNodeKind,
    reuse: LayoutNodeReuse,
    host: &'host mut TreeBuilderHost<'callbacks>,
    state: &'state mut TreeBuilderState,
    old_layout_node: LayoutNode,
    /// The node's identity in the style mirror, the document's own for the document.
    identity: StyleNodeID,
    /// The node's style node, which only an element or a text node has.
    style_node: Option<StyleNodeID>,
    element_type_facts: u32,
    context: &'context mut TreeBuilderContext,
    must_create_subtree: bool,
    insertion_mode: FfiInsertionMode,
}

struct PrincipalBoxConstruction {
    layout_node: LayoutNode,
    created_box: Option<UnplacedLayoutNode>,
    handled_display_contents: bool,
    /// The box replaces its element's contents with a single image, whose provider it owns.
    owns_content_replacement_image: bool,
}

impl PrincipalBoxConstruction {
    fn none() -> Self {
        Self {
            layout_node: NodeSlotId::INVALID,
            created_box: None,
            handled_display_contents: false,
            owns_content_replacement_image: false,
        }
    }
}

/// The box the element or text node `style_node` names is bound to, if it has one.
fn bound_box(arena: &LayoutNodeArena, style_node: u32) -> LayoutNode {
    match StyleNodeID::from_raw(style_node) {
        Some(style_node) => arena.bound_row(style_node),
        None => NodeSlotId::INVALID,
    }
}

/// The box of the pseudo-element `generated_for` on the element `node` is the box of. Rows of a
/// text node and anonymous rows name no element and have no pseudo-elements.
fn pseudo_element_box_of_element_box(layout: &TreeBuilderHost<'_>, node: LayoutNode, generated_for: u8) -> LayoutNode {
    let arena = layout.arena();
    if !arena.node_is_element_backed(node) {
        return NodeSlotId::INVALID;
    }
    arena.node_style_node(node).map_or(NodeSlotId::INVALID, |generator| {
        arena.bound_pseudo_element_row(generator, generated_for)
    })
}

fn construct_principal_layout_node(
    update: &mut PrincipalNodeUpdate<'_, '_, '_, '_>,
    should_create_layout_node: bool,
) -> PrincipalBoxConstruction {
    let host = &mut *update.host;
    let mut created_box = None;
    let mut owns_content_replacement_image = false;
    // The box this visit leaves the node with: the one it entered with when the node keeps it,
    // otherwise the one the host just built. Nothing between the entry and here rebinds the node.
    let mut layout_node = NodeSlotId::INVALID;
    let old_layout_node = update.old_layout_node;
    let must_create_subtree = update.must_create_subtree;
    let context = &mut *update.context;
    if update.kind.is_element() {
        if should_create_layout_node {
            // ::backdrop is a sibling of the element, not a child, so unlike other pseudo-elements, it is not
            // automatically discarded when the element's layout is recomputed.
            // A stale ::backdrop box is a viewport child, so removing it restructures the tree outside
            // every rebuild root.
            let old_backdrop = update.style_node.map_or(NodeSlotId::INVALID, |generator| {
                host.arena().bound_pseudo_element_row(generator, GENERATED_FOR_BACKDROP)
            });
            if !old_backdrop.is_invalid() {
                update.state.layout_tree_update_escaped_rebuild_roots = true;
                let backdrop_parent = host.parent(old_backdrop);
                assert!(!backdrop_parent.is_invalid());
                host.arena().detach_child(backdrop_parent, old_backdrop);
                host.free_subtree(old_backdrop);
            }
        }
        let element = update.identity;
        if should_create_layout_node {
            // The box is built again from scratch, so every pseudo-element box it holds goes.
            clear_synthetic_pseudo_element_boxes(host.host_calls(), host.arena, element);
        } else if host.arena().layout_tree_update_reuse_reasons(element)
            & layout_tree_update_reuse_reason::PSEUDO_ELEMENT_CHANGE
            != 0
        {
            // The box stays and only its generated content is regenerated, which is the ::before
            // and ::after boxes and nothing else.
            for generated_for in [GENERATED_FOR_BEFORE, GENERATED_FOR_AFTER] {
                let freed = free_pseudo_element_box(host.host_calls(), host.arena, element, generated_for);
                assert!(
                    freed != Some(false),
                    "a regenerated pseudo-element's box was not attached"
                );
            }
            let box_kept = host.arena().bound_row(element);
            if host.first_child(box_kept).is_invalid() {
                host.set_children_are_inline(box_kept, false);
            }
        }
        let published_record = || {
            host.arena()
                .with_style_store(|engine| engine.element_published_style_record(element))
        };
        let Some(record) = published_record() else {
            // NB: An existing box does not guarantee a current published style. Let the caller
            //     discard any stale box while the document settles the missing style.
            // Nothing published a style for the element, so a bypass path reached it without the
            // style update settling it. It gets no box in this build: the document styles it once
            // the build is over, and builds its box in the next one.
            update.state.reports.push(crate::layout::commit::FfiCommitMessage::new(
                element.raw(),
                crate::layout::commit::FfiCommitMessageKind::UnstyledElementReached,
            ));
            update.state.reached_unstyled_element = true;
            return PrincipalBoxConstruction::none();
        };
        // The record the box is built from is held for the whole build.
        update.state.pin_style_record_for_build(host.arena(), record);
        let display = host.published_display(update.style_node);
        let generation = principal_box_generation_decision(
            true,
            should_create_layout_node && display.is_none(),
            display.is_contents(),
        );
        if generation == PrincipalBoxGenerationDecision::Suppress {
            return PrincipalBoxConstruction::none();
        }
        if generation == PrincipalBoxGenerationDecision::DisplayContents {
            update_layout_tree_for_display_contents(
                host,
                update.state,
                update.identity,
                context,
                must_create_subtree,
                should_create_layout_node,
            );
            return PrincipalBoxConstruction {
                handled_display_contents: true,
                ..PrincipalBoxConstruction::none()
            };
        }
        if should_create_layout_node {
            let layout_kind = element_layout_kind(
                host.arena().published_content_is_single_image(update.style_node),
                update.element_type_facts,
                context.layout_svg_mask_or_clip_path,
                context.layout_svg_pattern,
            );
            let box_kind = host.arena().element_box_kind(update.style_node);
            // NB: A box's kind comes from the record its row is stamped with, not from the engine's newest assignment.
            //     The two differ while the element's host holds an older record than the engine assigned (a record that
            //     starts a CSS animation is installed only once the host applies the animation), and a row whose kind
            //     disagrees with its own style can't be laid out: a flex box styled display:none, e.g., forms no
            //     containing block for its children. Blink (LayoutTreeBuilderForElement::CreateLayoutObject), WebKit
            //     (RenderElement::createFor) and Gecko (nsCSSFrameConstructor::ConstructFrameFromItemInternal) likewise
            //     pick the type of a box from the one computed style that the box then carries. And that's per spec:
            //     For each element, "CSS generates zero or more boxes as specified by that element's display property",
            //     and "A box is assigned the same styles as its generating element".
            //     https://drafts.csswg.org/css-display-3/#intro
            let (record, facts) = host
                .arena()
                .element_box_style_record(update.style_node)
                .expect("an element the walk builds a box for has published its style");
            let kind = match layout_kind {
                FfiElementLayoutKind::ContentReplacement => Some(NodeKind::ImageBox),
                FfiElementLayoutKind::SvgMask => Some(NodeKind::SVGMaskBox),
                FfiElementLayoutKind::SvgClipPath => Some(NodeKind::SVGClipBox),
                FfiElementLayoutKind::SvgPattern => Some(NodeKind::SVGPatternBox),
                FfiElementLayoutKind::Normal => {
                    node_kind_for_element_box_kind(box_kind, facts.display, facts.appearance)
                }
            };
            if let Some(kind) = kind {
                let created = host.create_element_box(update.identity, kind, record);
                layout_node = created;
                created_box = Some(host.created(created));
                owns_content_replacement_image = layout_kind == FfiElementLayoutKind::ContentReplacement;
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
            layout_node = old_layout_node;
        }
    } else if should_create_layout_node {
        if update.kind.is_document() {
            let document_style = update
                .state
                .document_style
                .take()
                .expect("a build that builds the viewport is handed the document's style");
            let created = host.create_document_box(document_style);
            layout_node = created;
            created_box = Some(host.created(created));
        } else if update.kind.is_text() {
            let facts = host.arena().text_style_parent_facts(update.style_node);
            let needs_style_wrapper = display_contents_text_needs_style_wrapper(
                facts.has_style_parent,
                facts.parent_display_is_contents,
                host.arena().text_is_ascii_whitespace(update.style_node),
                facts.parent_collapses_whitespace,
            );
            let text_layout_node = host.create_text_box(update.identity);
            if needs_style_wrapper {
                let wrapper = host.create_anonymous_box_from_style_record(
                    facts.style_record,
                    AnonymousStyleKind::InlineStyleWrapper,
                    AnonymousStyleOverrides::default(),
                    NodeKind::InlineNode,
                );
                let wrapper_slot = wrapper.slot();
                host.set_children_are_inline(wrapper_slot, true);
                host.attach_child(wrapper_slot, host.created(text_layout_node), NodeSlotId::INVALID);
                layout_node = wrapper_slot;
                created_box = Some(wrapper);
            } else {
                layout_node = text_layout_node;
                created_box = Some(host.created(text_layout_node));
            }
        }
    } else {
        layout_node = old_layout_node;
    }

    PrincipalBoxConstruction {
        layout_node,
        created_box,
        handled_display_contents: false,
        owns_content_replacement_image,
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
    let containing_block = arena.containing_block_by_walking_ancestors(old_layout_node);
    if containing_block.is_invalid() {
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
    entry_facts: PrincipalNodeEntryFacts,
    entry_decision: PrincipalNodeEntryDecision,
) {
    let prior_has_svg_root = update.context.has_svg_root;
    match entry_decision.svg {
        SvgEntryDecision::EnterSvgRoot => update.context.has_svg_root = true,
        SvgEntryDecision::EnterForeignContent => update.context.has_svg_root = false,
        SvgEntryDecision::Continue | SvgEntryDecision::Skip => {}
    }

    let construction = if entry_decision.svg == SvgEntryDecision::Skip {
        PrincipalBoxConstruction::none()
    } else {
        construct_principal_layout_node(update, entry_decision.should_create_layout_node)
    };
    let host = &mut *update.host;
    let mut created_box = construction.created_box;
    let context = &mut *update.context;

    if !construction.layout_node.is_invalid() {
        let layout_node = construction.layout_node;
        if update.kind.is_element() || update.kind.is_document() {
            host.arena().owe_image_resources(
                layout_node,
                OwedImageResources::StyleResources {
                    owns_content_replacement_image: construction.owns_content_replacement_image,
                },
            );
        }

        let starts_new_subtree = entry_decision.should_create_layout_node && update.state.new_subtree_root.is_invalid();
        if starts_new_subtree {
            update.state.new_subtree_root = layout_node;
        }
        if entry_facts.needs_layout_tree_update
            && (update.reuse.insert_children || update.reuse.update_pseudo_elements)
            && !entry_decision.should_create_layout_node
        {
            update.state.reused_child_list_update_roots.push(layout_node);
        }
        let adjustment = replaced_element_display_adjustment(host, layout_node);
        if adjustment != FfiReplacedElementDisplayAdjustment::None {
            apply_replaced_display_adjustment(host.host_calls(), host.arena(), layout_node, adjustment);
        }

        let old_layout_node = update.old_layout_node;
        let placement_facts = PrincipalBoxPlacementFacts {
            must_create_subtree: update.must_create_subtree,
            should_create_layout_node: entry_decision.should_create_layout_node,
            has_old_layout_node: !old_layout_node.is_invalid(),
            old_layout_node_is_attached: !old_layout_node.is_invalid() && !host.parent(old_layout_node).is_invalid(),
            old_and_new_layout_nodes_are_same: old_layout_node == layout_node,
            has_current_rebuild_root: !update.state.current_rebuild_root.is_invalid(),
            is_in_dom_order_insertion: update.insertion_mode == FfiInsertionMode::InDomOrder,
            is_document: update.kind.is_document(),
            is_element: update.kind.is_element(),
            rendered_in_top_layer: update.element_type_facts & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0,
        };
        let layout_node_is_svg_box = node_facts::kind_is_svg_box(host.data(layout_node).kind.get());
        let prior_layout_top_layer = context.layout_top_layer;
        let placement =
            principal_box_placement_decision(placement_facts, layout_node_is_svg_box, prior_layout_top_layer);

        let mut prior_rebuild_root = NodeSlotId::INVALID;
        if placement.start_rebuild_root {
            prior_rebuild_root = update.state.current_rebuild_root;
            update.state.current_rebuild_root = layout_node;
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
            let unplaced_backdrop = create_pseudo_element(
                host,
                update.state,
                update.identity,
                FfiPseudoElement::Backdrop,
                insertion_mode,
            );
            if let Some(backdrop) = unplaced_backdrop {
                assert!(placement.may_replace_existing_layout_node);
                let topmost_placement = topmost_layout_node_of_top_layer_placement(host.arena(), old_layout_node);
                let old_placement = if topmost_placement.is_invalid() {
                    old_layout_node
                } else {
                    topmost_placement
                };
                let old_parent = host.parent(old_placement);
                assert!(!old_parent.is_invalid());
                // The backdrop lands next to the still-attached old placement, so this restructures
                // its parent.
                note_layout_tree_restructuring_at(host, update.state, old_parent);
                host.attach_child(old_parent, backdrop, old_placement);
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
                let is_inline_outside = node_is_inline_outside(host, layout_node);
                insert_node_into_inline_or_block_ancestor(
                    host,
                    update.state,
                    current_parent,
                    created_box.take().expect("a principal box to place"),
                    is_inline_outside,
                    update.insertion_mode,
                    Some(update.identity),
                );
            }
            FfiPrincipalBoxPlacement::AppendSvg => {
                assert!(!current_parent.is_invalid());
                host.attach_child(
                    current_parent,
                    created_box.take().expect("a principal box to place"),
                    NodeSlotId::INVALID,
                );
            }
            FfiPrincipalBoxPlacement::ReplaceExisting => {
                let arena = host.arena();
                let old_data = arena.data(old_layout_node);
                let new_data = arena.data(layout_node);
                if node_facts::kind_is_box(old_data.kind.get())
                    && node_facts::kind_is_box(new_data.kind.get())
                    && let Some(link) = arena.take_committed_fragment_link(old_data)
                {
                    arena.set_committed_fragment_link(new_data, link, None);
                }
                transfer_fragments_to_replacement_box(arena, old_layout_node, layout_node);
                super::layout_node_arena::prepare_subtree_for_detach(host.host_calls(), arena, old_layout_node);
                let old_parent = host.parent(old_layout_node);
                assert!(!old_parent.is_invalid());
                let replaced_old_box = arena.replace_child(
                    old_parent,
                    old_layout_node,
                    created_box.take().expect("a principal box to place"),
                );
                host.free_subtree(replaced_old_box);
            }
            FfiPrincipalBoxPlacement::DocumentRoot => {
                host.arena().set_layout_root(layout_node);
                if let Some(viewport) = created_box.take() {
                    viewport.placed_as_layout_root();
                }
            }
            FfiPrincipalBoxPlacement::None => assert!(created_box.is_none()),
        }
        update_principal_node_descendants(
            host,
            update.state,
            construction.layout_node,
            context,
            PrincipalDescendantUpdate {
                kind: update.kind,
                style_node: update.style_node,
                mirror_identity: update.identity,
                element_type_facts: update.element_type_facts,
                should_create_layout_node: entry_decision.should_create_layout_node,
                update_pseudo_elements_in_place: update.reuse.update_pseudo_elements
                    && !entry_decision.should_create_layout_node,
                must_create_subtree: update.must_create_subtree,
                insertion_mode: if update.reuse.insert_children {
                    FfiInsertionMode::InDomOrder
                } else {
                    FfiInsertionMode::Append
                },
            },
        );

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
            let old_parent = host.parent(update.old_layout_node);
            if !old_parent.is_invalid() {
                update.state.additional_table_fixup_roots.push(old_parent);
            }
        }
        // If no layout node was created, remove every stale layout and paint node from the shadow-including subtree.
        clear_stale_subtree(host, update.identity, StaleSubtreeClearScope::Inclusive);
    }

    if matches!(
        entry_decision.svg,
        SvgEntryDecision::EnterSvgRoot | SvgEntryDecision::EnterForeignContent
    ) {
        context.has_svg_root = prior_has_svg_root;
    }
}

/// Updates the node an identity names, and its layout-tree subtree.
fn update_layout_tree(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    identity: StyleNodeID,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
    insertion_mode: FfiInsertionMode,
) {
    update_layout_tree_from(
        host,
        state,
        identity,
        context,
        must_create_subtree,
        insertion_mode,
        false,
    );
}

/// As [`update_layout_tree`], for the document the build starts from when `is_document_root`.
fn update_layout_tree_from(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    identity: StyleNodeID,
    context: &mut TreeBuilderContext,
    must_create_subtree: bool,
    insertion_mode: FfiInsertionMode,
    is_document_root: bool,
) {
    let kind = PrincipalNodeKind::of(identity, is_document_root);
    // The document's identity only roots the style mirror's child sequence; it has no style.
    let style_node = (!kind.is_document()).then_some(identity);

    // The box the node already has is the row the arena binds to its identity; the document is
    // bound through the viewport row instead.
    let old_layout_node = if kind.is_document() {
        host.arena().bound_viewport_row()
    } else {
        host.arena().bound_row(identity)
    };
    let entry_facts = PrincipalNodeEntryFacts {
        must_create_subtree,
        needs_layout_tree_update: host.arena().needs_layout_tree_update(identity),
        has_layout_node: !old_layout_node.is_invalid(),
        layout_node_is_attached: !old_layout_node.is_invalid() && !host.parent(old_layout_node).is_invalid(),
    };

    let reuse = resolve_layout_node_reuse(host, kind, style_node);
    let element_type_facts = host.element_type_facts(style_node);
    let entry_decision = principal_node_entry_decision(entry_facts, reuse, kind, element_type_facts, context);
    if entry_decision.top_layer != TopLayerEntryDecision::Continue {
        if entry_decision.top_layer == TopLayerEntryDecision::SkipAndRequestZoneRebuild {
            // A member found here without an attached box was cleared together with a hidden ancestor subtree, and
            // nothing is scheduled to rebuild it. Request another top-layer zone pass instead of stranding dirty
            // flags below ancestors whose walks already finished.
            state.reports.push(crate::layout::commit::FfiCommitMessage::new(
                0,
                crate::layout::commit::FfiCommitMessageKind::TopLayerZoneRebuildNeeded,
            ));
        }
        return;
    }

    let mut update = PrincipalNodeUpdate {
        kind,
        reuse,
        host,
        state,
        old_layout_node,
        identity,
        style_node,
        element_type_facts,
        context,
        must_create_subtree,
        insertion_mode,
    };
    update_principal_node_after_entry(&mut update, entry_facts, entry_decision);
}

/// What a layout tree build leaves for its caller: the viewport the tree hangs from and how
/// confined the rebuild stayed. The rebuilt subtree roots themselves wait in the arena for the
/// partial relayout plan that follows the build.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutTreeBuildOutcome {
    pub viewport: NodeSlotId,
    pub rebuilt_subtree_root_count: usize,
    pub layout_tree_update_escaped_rebuild_roots: bool,
    pub needs_another_build_pass: bool,
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
pub enum FfiPseudoElementDecision {
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
    /// Whether the pseudo-element's box is an ordinary inline box, which is the one kind whose
    /// empty generated text still has to exist.
    pub display_is_inline_flow: bool,
    pub has_content_replacement: bool,
    /// The originating element's box when it is a list item box, for a ::marker.
    pub originating_list_box: NodeSlotId,
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

pub(crate) fn pseudo_element_decision(facts: PseudoElementFacts) -> FfiPseudoElementDecision {
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
            ComputedContentType::Normal | ComputedContentType::None
        )
    {
        return FfiPseudoElementDecision::None;
    }

    // For ::marker with content 'none' -- do nothing.
    if facts.pseudo_element == FfiPseudoElement::Marker && facts.content_type == ComputedContentType::None {
        return FfiPseudoElementDecision::None;
    }

    if facts.pseudo_element == FfiPseudoElement::Marker
        && facts.content_type == ComputedContentType::Normal
        && !facts.originating_list_box.is_invalid()
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
}

/// Resolves the CSS counters set of `element`, or of one of its pseudo-elements, now that its box
/// is in the layout tree, and answers whose set it is.
fn resolve_counters(
    host: &TreeBuilderHost<'_>,
    element: StyleNodeID,
    pseudo_element: FfiPseudoElement,
) -> crate::layout::counters::CounterOwner {
    let owner = counter_owner_of_pseudo_element(element, pseudo_element);
    crate::layout::counters::resolve_counters(host.arena(), owner);
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

/// Tells the document the content of `owner` shows the `list-item` counter's value.
fn report_list_item_counter_rendering(
    state: &mut TreeBuilderState,
    owner: crate::layout::counters::CounterOwner,
    renders_list_item_counter_value: bool,
) {
    if renders_list_item_counter_value {
        state.reports.push(crate::layout::commit::FfiCommitMessage::new(
            owner.element.raw(),
            crate::layout::commit::FfiCommitMessageKind::ListItemCounterValueRendered,
        ));
    }
}

/// What the style mirror published for the pseudo-element `pseudo_element` on `element`, for the
/// build to decide whether it gets a box.
fn published_pseudo_element_facts(
    layout: &TreeBuilderHost<'_>,
    element: StyleNodeID,
    pseudo_element: FfiPseudoElement,
) -> PseudoElementFacts {
    let facts = PseudoElementFacts {
        has_style: false,
        pseudo_element,
        content_type: ComputedContentType::None,
        display_is_none: false,
        display_is_contents: false,
        display_is_list_item: false,
        display_is_inline_flow: false,
        has_content_replacement: false,
        originating_list_box: NodeSlotId::INVALID,
        normal_marker_has_content: false,
        marker_position_is_inside: false,
    };
    let published = layout.arena().with_style_store(|engine| {
        engine
            .pseudo_published_style_view(element, pseudo_kind_of(generated_for_of(pseudo_element)))
            .map(|view| {
                let display = view.display();
                PseudoElementFacts {
                    has_style: true,
                    content_type: if !view.content_is_keyword() {
                        ComputedContentType::List
                    } else if view.content_keyword_is_none() {
                        ComputedContentType::None
                    } else {
                        ComputedContentType::Normal
                    },
                    display_is_none: display.is_none(),
                    display_is_contents: display.is_contents(),
                    display_is_list_item: display.is_list_item(),
                    display_is_inline_flow: display.is_inline_outside() && display.is_flow_inside(),
                    has_content_replacement: view.content_is_single_image(),
                    ..facts
                }
            })
    });
    // A pseudo-element the mirror holds no record for generates nothing.
    let Some(mut facts) = published else {
        return facts;
    };
    // A ::marker belongs to the list item box its element was built as, and takes its position and
    // its default content from that box's style.
    if pseudo_element == FfiPseudoElement::Marker {
        let originating_box = layout.arena().bound_row(element);
        if !originating_box.is_invalid() && layout.data(originating_box).kind.get() == NodeKind::ListItemBox {
            facts.originating_list_box = originating_box;
            if let Some(list_style) = layout.style(originating_box) {
                facts.normal_marker_has_content =
                    !list_style.list_style_type_is_none() || list_style.list_style_image_is_set();
                facts.marker_position_is_inside = list_style.list_style_position_is_inside();
            }
        }
    }
    facts
}

/// The row a pseudo-element's box is built in, stamped from the record the style mirror published for
/// the pseudo-element, and its layout node; none for a display that generates no box.
fn stamp_pseudo_element_box_row(
    host: &mut TreeBuilderHost<'_>,
    generator: StyleNodeID,
    pseudo_element: FfiPseudoElement,
    decision: FfiPseudoElementDecision,
    facts: PseudoElementFacts,
) -> Option<NodeSlotId> {
    let generated_for = generated_for_of(pseudo_element);
    let is_list_item_marker = decision == FfiPseudoElementDecision::Box && !facts.originating_list_box.is_invalid();
    let kind = match decision {
        FfiPseudoElementDecision::None => unreachable!("a pseudo-element that generates nothing gets no box"),
        // The image box owns the image it replaces the pseudo-element's contents with.
        FfiPseudoElementDecision::ContentReplacement => NodeKind::ImageBox,
        // https://drafts.csswg.org/css-content-3/#content-property
        // A pseudo-element whose contents are a content list is an inline box holding them.
        FfiPseudoElementDecision::Contents => NodeKind::InlineNode,
        FfiPseudoElementDecision::Box if is_list_item_marker => NodeKind::ListItemMarkerBox,
        FfiPseudoElementDecision::Box => host
            .arena()
            .with_style_store(|engine| {
                engine
                    .pseudo_published_style_view(generator, pseudo_kind_of(generated_for))
                    .map(|view| view.display())
            })
            .and_then(node_kind_for_display)?,
    };
    let slot = host.arena.allocate_unbound();
    host.arena()
        .stamp_pseudo_element_row(slot, kind, generator, generated_for);
    if decision == FfiPseudoElementDecision::Contents {
        host.arena().update_layout_style(host.host_calls(), slot, |style| {
            style.set_display(FfiDisplay::outside_and_inside(
                crate::css::css_enums::display_outside::INLINE,
                crate::css::css_enums::display_inside::FLOW,
                false,
            ));
        });
    }
    if is_list_item_marker {
        host.arena()
            .set_node_flag(slot, NodeFlag::ListMarkerIsInside, facts.marker_position_is_inside);
    }
    host.note_style_of_built_row(slot, None);
    Some(slot)
}

/// The row the list marker a list-item pseudo-element nests is built in, and its layout node. The
/// marker belongs to the pseudo-element that nests it, not to the generator's own `::marker`, so it
/// is generated for that pseudo-element and never becomes the `::marker`'s box.
fn stamp_nested_list_marker_row(
    host: &mut TreeBuilderHost<'_>,
    generator: StyleNodeID,
    pseudo_element: FfiPseudoElement,
    list_item_box: NodeSlotId,
) -> NodeSlotId {
    let derived = host.arena().with_style_engine(|engine| {
        // The generator's own `::marker` record, which the engine derives for this read alone where the generator
        // holds none. It answers one for every list item; should it not, the marker takes the generator's style.
        let record = engine
            .pseudo_published_style_record(generator, pseudo_kind_of(GENERATED_FOR_MARKER))
            .or_else(|| {
                let demand = RecordDemand::PseudoElement(
                    FfiPseudoElementRecordDemand::ReadOnly,
                    FfiDemandedPseudoElement::Marker,
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
    host.arena()
        .stamp_anonymous_box(slot, NodeKind::ListItemMarkerBox, derived);
    host.arena()
        .set_node_generated_for(slot, generated_for_of(pseudo_element), Some(generator));
    let marker_position_is_inside = host
        .style(list_item_box)
        .is_some_and(ComputedValuesView::list_style_position_is_inside);
    host.arena()
        .set_node_flag(slot, NodeFlag::ListMarkerIsInside, marker_position_is_inside);
    host.note_style_of_built_row(slot, None);
    host.arena().owe_image_resources(
        slot,
        OwedImageResources::StyleResources {
            owns_content_replacement_image: false,
        },
    );
    slot
}

/// The row one item of a pseudo-element's generated content is built in, and its layout node. An
/// image item takes the style of `style_box`, the pseudo-element's box or the marker it nests, as
/// an inline box.
fn create_generated_content_item(
    host: &mut TreeBuilderHost<'_>,
    generator: StyleNodeID,
    pseudo_element: FfiPseudoElement,
    item: crate::layout::generated_content::ContentItem,
    style_box: NodeSlotId,
) -> NodeSlotId {
    use crate::layout::generated_content::ContentItem;
    let image = match item {
        ContentItem::Text(text) => {
            let slot = host.stamp_generated_text_box(&text);
            host.arena()
                .set_node_generated_for(slot, generated_for_of(pseudo_element), Some(generator));
            return slot;
        }
        ContentItem::Image(content_index) => FfiGeneratedImage {
            kind: FfiGeneratedImageKind::ContentImage,
            content_index,
            marker: NodeSlotId::INVALID,
        },
        ContentItem::ListStyleImage => FfiGeneratedImage {
            kind: FfiGeneratedImageKind::ListStyleImage,
            content_index: 0,
            marker: style_box,
        },
    };
    // https://drafts.csswg.org/css-content-3/#content-property
    // For <image>, this is an inline anonymous replaced element.
    let derived = host.arena().derive_style_record_with_display(
        host.arena().node_style_record(style_box),
        FfiDisplay::outside_and_inside(
            crate::css::css_enums::display_outside::INLINE,
            crate::css::css_enums::display_inside::FLOW,
            false,
        ),
    );
    let slot = host.arena.allocate_unbound();
    host.arena().stamp_anonymous_box(slot, NodeKind::ImageBox, derived);
    host.arena()
        .set_node_generated_for(slot, generated_for_of(pseudo_element), Some(generator));
    host.arena().owe_image_resources(
        slot,
        OwedImageResources::GeneratedImage {
            generator,
            pseudo_element,
            image,
        },
    );
    slot
}

fn create_pseudo_element(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    element_identity: StyleNodeID,
    pseudo_element: FfiPseudoElement,
    insertion_mode: Option<FfiInsertionMode>,
) -> Option<UnplacedLayoutNode> {
    // The record the pseudo-element's boxes are built from is held for the whole build.
    let record = host.arena().with_style_store(|engine| {
        engine.pseudo_published_style_record(element_identity, pseudo_kind_of(generated_for_of(pseudo_element)))
    });
    if let Some(record) = record {
        state.pin_style_record_for_build(host.arena(), record);
    }
    // The pseudo-element gives up the box it holds from an earlier build before the walk decides
    // whether it gets a new one.
    host.arena()
        .clear_pseudo_element_box(element_identity, generated_for_of(pseudo_element));
    let facts = published_pseudo_element_facts(host, element_identity, pseudo_element);
    let decision = pseudo_element_decision(facts);
    if decision == FfiPseudoElementDecision::None {
        return None;
    }

    let layout_node = stamp_pseudo_element_box_row(host, element_identity, pseudo_element, decision, facts)?;
    let mut unplaced_box = Some(host.created(layout_node));

    // https://drafts.csswg.org/css-lists-3/#list-style-position-outside
    // "the marker box is a block container and is placed outside the principal block box"
    if decision == FfiPseudoElementDecision::Box
        && !facts.originating_list_box.is_invalid()
        && !facts.marker_position_is_inside
    {
        let list_item_box = facts.originating_list_box;
        assert_eq!(host.data(list_item_box).kind.get(), NodeKind::ListItemBox);
        let first_child = host.first_child(list_item_box);
        host.attach_child(list_item_box, unplaced_box.take().expect("the marker box"), first_child);
    }

    host.arena()
        .stamp_pseudo_element_box(layout_node, element_identity, generated_for_of(pseudo_element));
    host.arena().owe_image_resources(
        layout_node,
        OwedImageResources::StyleResources {
            owns_content_replacement_image: decision == FfiPseudoElementDecision::ContentReplacement,
        },
    );
    if decision == FfiPseudoElementDecision::ContentReplacement {
        let adjustment = replaced_element_display_adjustment(host, layout_node);
        if adjustment != FfiReplacedElementDisplayAdjustment::None {
            apply_replaced_display_adjustment(host.host_calls(), host.arena(), layout_node, adjustment);
        }
    }

    let initial_quote_nesting_level = state.quote_nesting_level;
    let layout_node_kind = host.data(layout_node).kind.get();
    let is_outside_marker = layout_node_kind == NodeKind::ListItemMarkerBox && !facts.marker_position_is_inside;
    if let Some(insertion_mode) = insertion_mode
        && !is_outside_marker
    {
        let current_parent = state.current_parent();
        let is_inline_outside = node_is_inline_outside(host, layout_node);
        insert_node_into_inline_or_block_ancestor(
            host,
            state,
            current_parent,
            unplaced_box.take().expect("the pseudo-element box"),
            is_inline_outside,
            insertion_mode,
            None,
        );
    }
    let owner = resolve_counters(host, element_identity, pseudo_element);

    // FIXME: This code actually computes style for element::marker, and shouldn't for element::pseudo::marker.
    if layout_node_kind == NodeKind::ListItemBox {
        let marker_slot = stamp_nested_list_marker_row(host, element_identity, pseudo_element, layout_node);
        let marker = host.created(marker_slot);
        let first_child = host.first_child(layout_node);
        host.attach_child(layout_node, marker, first_child);
        let marker_content = crate::layout::generated_content::resolve_nested_marker_content(
            host.arena(),
            owner,
            marker_slot,
            layout_node,
        );
        report_list_item_counter_rendering(state, owner, marker_content.renders_list_item_counter_value);
        let content =
            create_generated_content_item(host, element_identity, pseudo_element, marker_content.item, marker_slot);
        host.attach_child(marker_slot, host.created(content), NodeSlotId::INVALID);
        host.set_children_are_inline(marker_slot, true);
    }

    // Resolve content after insertion because counter() and counters() items read the counters established by this
    // pseudo-element's box.
    let marker_and_list_box = (layout_node_kind == NodeKind::ListItemMarkerBox).then(|| {
        assert!(
            !facts.originating_list_box.is_invalid(),
            "a list marker box belongs to a list item box"
        );
        (layout_node, facts.originating_list_box)
    });
    let resolved_content = crate::layout::generated_content::resolve_content(
        host.arena(),
        owner,
        marker_and_list_box,
        initial_quote_nesting_level,
    );
    report_list_item_counter_rendering(state, owner, resolved_content.renders_list_item_counter_value);
    state.quote_nesting_level = resolved_content.final_quote_nesting_level;

    if resolved_content.is_list && decision != FfiPseudoElementDecision::ContentReplacement {
        state.ancestor_stack.push(layout_node);
        for item in resolved_content.items {
            // An empty generated text node carries the inline fragment of an ordinary inline
            // pseudo-element. Other pseudo-element boxes exist independently of their contents, so
            // avoid giving them a zero-length child that would force layout to measure an
            // otherwise empty box.
            if !facts.display_is_inline_flow
                && matches!(&item, crate::layout::generated_content::ContentItem::Text(text) if text.is_empty())
            {
                continue;
            }
            let content_item = create_generated_content_item(host, element_identity, pseudo_element, item, layout_node);
            let current_parent = state.current_parent();
            let is_inline_outside = node_is_inline_outside(host, content_item);
            insert_node_into_inline_or_block_ancestor(
                host,
                state,
                current_parent,
                host.created(content_item),
                is_inline_outside,
                FfiInsertionMode::Append,
                None,
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
    if !node_facts::has_flag(host.data(node), NodeFlag::IsReplacedElement) {
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
}

#[derive(Clone, Copy)]
pub struct FirstLetterTarget {
    pub text_layout_node: NodeSlotId,
    pub letter_end: usize,
    pub source_length: usize,
    pub found: bool,
}

impl FirstLetterTarget {
    fn not_found() -> Self {
        Self {
            text_layout_node: NodeSlotId::INVALID,
            letter_end: 0,
            source_length: 0,
            found: false,
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

#[unsafe(no_mangle)]
pub extern "C" fn layout_node_kind_is_svg_box(kind: NodeKind) -> bool {
    node_facts::kind_is_svg_box(kind)
}

fn node_is_generated_for_pseudo_element(data: &NodeData) -> bool {
    data.generated_for.get() != 0
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

fn node_has_replaced_element_table_display_adjustment(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    node_facts::has_flag(host.data(node), NodeFlag::IsReplacedElement)
        && host.style(node).is_some_and(|style| {
            let display = style.display_before_box_type_transformation();
            display.is_table_inside() || display.is_internal_table() || display.is_table_caption()
        })
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
        self.arena
            .style_payloads(node)
            .map(|payloads| ComputedValuesView::new(&payloads.groups))
    }

    fn display(&self, node: LayoutNode) -> FfiDisplay {
        self.style(node).map_or_else(FfiDisplay::block, |style| style.display())
    }

    fn display_before_box_type_transformation(&self, node: LayoutNode) -> FfiDisplay {
        self.style(node).map_or_else(FfiDisplay::block, |style| {
            style.display_before_box_type_transformation()
        })
    }

    fn set_children_are_inline(&self, node: LayoutNode, children_are_inline: bool) {
        self.arena()
            .set_node_flag(node, NodeFlag::ChildrenAreInline, children_are_inline);
    }

    fn arena(&self) -> &LayoutNodeArena {
        self.arena
    }

    fn created(&self, slot: NodeSlotId) -> UnplacedLayoutNode {
        UnplacedLayoutNode::new(slot)
    }

    /// The row an element's principal box of `kind` is built in, stamped with `record`, the style
    /// record the element published. A fieldset and a media element adjust their box as it is built.
    fn create_element_box(&mut self, element: StyleNodeID, kind: NodeKind, record: u64) -> NodeSlotId {
        let slot = self.stamp_dom_box(kind, Some(element));
        self.arena().stamp_published_style(slot, record);
        match kind {
            // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
            // If the computed outer display type is inline, the fieldset is expected to behave as inline-block.
            // Otherwise, it is expected to behave as flow-root. This does not change the computed value.
            NodeKind::FieldSetBox => {
                let display = self.style(slot).map(|style| style.display());
                if let Some(display) = display.filter(FfiDisplay::is_flow_inside) {
                    self.arena().update_layout_style(self.host_calls(), slot, |style| {
                        style.set_display(FfiDisplay::outside_and_inside(
                            display.outside,
                            crate::css::css_enums::display_inside::FLOW_ROOT,
                            false,
                        ));
                    });
                }
            }
            // A media element renders the children of its shadow root, such as its controls.
            NodeKind::AudioBox | NodeKind::VideoBox => {
                let has_shadow_root = self.arena().shadow_root_of(element).is_some();
                self.arena()
                    .set_node_flag(slot, NodeFlag::ReplacedBoxCanHaveChildren, has_shadow_root);
            }
            _ => {}
        }
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
        self.arena().stamp_dom_row(slot, kind, style_node);
        self.arena().take_over_rows_of_bound_node(slot);
        slot
    }

    /// The row a text node's box is built in, stamped out of the text node's identity.
    fn create_text_box(&mut self, style_node: StyleNodeID) -> NodeSlotId {
        let slot = self.stamp_dom_box(NodeKind::TextNode, Some(style_node));
        self.stamp_text_row_facts(slot, style_node);
        slot
    }

    /// What giving a row its style tells the rest of the document: which scroll containers
    /// snapping may happen in, with the root element's box standing in for the viewport, and what
    /// an element's highlight pseudo-element styles paint highlighted text with. None of it needs
    /// the row's layout node.
    fn note_style_of_built_row(&self, slot: NodeSlotId, element: Option<StyleNodeID>) {
        if let Some(element) = element
            && self.holds_highlight_style(element)
        {
            crate::painting::selection::note_built_row_highlight_pseudo_styles(self.arena(), slot, element);
        }
        if self.arena().node_flags(slot) & NodeFlag::IsDocumentElement as u32 != 0 {
            let viewport = self.arena().bound_viewport_row();
            if !viewport.is_invalid() {
                self.arena().note_built_scroll_container(viewport);
            }
        } else if node_facts::kind_and_style_make_scroll_container(self.data(slot).kind.get(), self.style(slot)) {
            self.arena().note_built_scroll_container(slot);
        }
    }

    /// A text row is stamped with whether an empty text produces a line box fragment, which text
    /// controls and editing hosts rely on: the fragment keeps the line box alive with real font
    /// metrics, giving the caret an anchor to paint at and the control its baseline. The document
    /// restamps it when editability changes. Under an element with a highlight pseudo-element style
    /// and no box of its own, the row takes that style's paint facts itself.
    fn stamp_text_row_facts(&self, slot: NodeSlotId, text: StyleNodeID) {
        use crate::css::style::bridge::element_construction_fact::{IS_EDITING_HOST, IS_HTML_INPUT_ELEMENT};
        let is_in_user_agent_shadow_tree =
            self.arena().node_flags(slot) & NodeFlag::IsInUserAgentShadowTree as u32 != 0;
        let (produces_line_box_fragment_when_empty, parent_element) = self.arena().with_style_store(|engine| {
            let tree = engine.tree();
            let parent = tree.text_parent(text);
            let parent_element = parent.filter(|parent| parent.element_index().is_some());
            let parent_is_editing_host =
                parent_element.is_some_and(|parent| engine.element_construction_facts(parent) & IS_EDITING_HOST != 0);
            // NB: Tree relations are keyed by element identity, so the walk to the host starts at the
            //     text's parent.
            let is_in_text_control = is_in_user_agent_shadow_tree
                && parent
                    .and_then(|parent| tree.shadow_host_of(parent))
                    .is_some_and(|host| {
                        engine.element_construction_facts(host) & IS_HTML_INPUT_ELEMENT != 0
                            || engine.element_box_kind(host) == ElementBoxKind::TextArea
                    });
            (parent_is_editing_host || is_in_text_control, parent_element)
        });
        self.arena().set_node_flag(
            slot,
            NodeFlag::ProducesLineBoxFragmentWhenEmpty,
            produces_line_box_fragment_when_empty,
        );
        if let Some(parent) = parent_element
            && self.holds_highlight_style(parent)
            && self.arena().bound_row(parent).is_invalid()
        {
            crate::painting::selection::note_built_row_highlight_pseudo_styles(self.arena(), slot, parent);
        }
    }

    /// Whether the element published a record for a highlight pseudo-element. An element whose own
    /// rules style no `::selection` still holds one inherited from an ancestor's, which the
    /// published pseudo-element mask does not show.
    fn holds_highlight_style(&self, element: StyleNodeID) -> bool {
        self.arena().with_style_store(|engine| {
            crate::painting::selection::HighlightPseudoElement::ALL
                .iter()
                .any(|highlight| {
                    engine
                        .pseudo_published_style_record(element, highlight.pseudo_kind())
                        .is_some()
                })
        })
    }

    /// The row the document's viewport is built in, stamped out of its kind and the document's
    /// style the build was handed.
    fn create_document_box(&mut self, document_style: DerivedStyleRecord) -> NodeSlotId {
        let slot = self.stamp_dom_box(NodeKind::Viewport, None);
        // The viewport is the document's row, painted with what the document published and named
        // by the document's name.
        let document = self
            .arena()
            .document_style_node()
            .expect("a build names the document it lays out");
        self.arena().stamp_dom_paint_facts(slot, document);
        let unique_node_id = self
            .arena()
            .with_style_store(|engine| engine.element_unique_node_id(document));
        self.arena().unique_node_ids().publish(slot, unique_node_id);
        self.arena()
            .apply_reinherited_style_record(self.host_calls(), slot, document_style);
        self.arena().note_built_scroll_container(slot);
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
            .arena()
            .derive_anonymous_style_record(parent_style_record, style_kind, overrides);
        let slot = self.arena.allocate_unbound();
        self.arena().stamp_anonymous_box(slot, node_kind, derived);
        self.arena().refresh_insets_use_anchor_functions_flag(slot);
        // An anonymous inline box takes its style from its parent, so it may name images too.
        if node_kind == NodeKind::InlineNode {
            self.arena().owe_image_resources(
                slot,
                OwedImageResources::StyleResources {
                    owns_content_replacement_image: false,
                },
            );
        }
        UnplacedLayoutNode::new(slot)
    }

    fn anonymous_wrapper_overrides(&self, parent: LayoutNode) -> AnonymousStyleOverrides {
        AnonymousStyleOverrides {
            inline_block_wrapper: self.display(parent).is_inline_block() && self.first_child(parent).is_invalid(),
            ..AnonymousStyleOverrides::default()
        }
    }

    fn create_anonymous_wrapper_box(&mut self, parent: LayoutNode) -> UnplacedLayoutNode {
        self.create_anonymous_box(
            parent,
            AnonymousStyleKind::Wrapper,
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

    fn last_child(&self, node: LayoutNode) -> LayoutNode {
        self.data(node).last_child.get()
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
            self.arena().detach_child(parent, node);
        }
        for &node in nodes {
            self.free_subtree(node);
        }
    }

    fn wrap_in_anonymous(&mut self, nodes: &[LayoutNode], nearest_sibling: LayoutNode, kind: FfiAnonymousTableBoxKind) {
        assert!(!nodes.is_empty());
        let parent = self.parent(nodes[0]);
        assert!(!parent.is_invalid());
        let (style_kind, node_kind) = match kind {
            FfiAnonymousTableBoxKind::TableRow => (AnonymousStyleKind::TableRow, NodeKind::Box),
            FfiAnonymousTableBoxKind::TableCell => (AnonymousStyleKind::TableCell, NodeKind::BlockContainer),
            FfiAnonymousTableBoxKind::Table => (AnonymousStyleKind::Table, NodeKind::Box),
            FfiAnonymousTableBoxKind::InlineTable => (AnonymousStyleKind::InlineTable, NodeKind::Box),
        };
        let wrapper = self.create_anonymous_box(parent, style_kind, AnonymousStyleOverrides::default(), node_kind);
        let wrapper_slot = wrapper.slot();
        for &node in nodes {
            self.move_child(node, wrapper_slot, NodeSlotId::INVALID);
        }
        // An anonymous table-cell takes over the run of content it is generated around, so it has inline children
        // exactly when that run is inline-level. Anonymous table-row, table and inline-table boxes only ever own
        // table-internal boxes (their wrapped children are, or become through the remaining fixup steps, table-internal
        // boxes), so they never have inline children, not even when they are generated inside an inline box. An
        // inline-table with inline children would also not derive its baseline from its first row.
        let children_are_inline = match kind {
            FfiAnonymousTableBoxKind::TableCell => nodes.iter().any(|&node| node_is_inline_outside(self, node)),
            FfiAnonymousTableBoxKind::TableRow
            | FfiAnonymousTableBoxKind::Table
            | FfiAnonymousTableBoxKind::InlineTable => false,
        };
        self.set_children_are_inline(wrapper_slot, children_are_inline);
        self.attach_child(parent, wrapper, nearest_sibling);
        // The wrapper takes the place of the run in its parent. A table-row, table-cell or table box is block-level,
        // so a parent whose inline content it replaced entirely (a table-row-group or table around text, a table-row
        // around text or inline boxes) no longer has inline children. A stale flag would make derive_baselines() look
        // for line boxes the parent does not have, leaving it and every box that derives its baseline from it (a cell
        // around a nested table, an inline-table in a line) without a baseline. A parent that keeps inline-level
        // children next to the wrapper (text around an anonymous table generated for out-of-flow row groups, which
        // the fixup treats as inline-level boxes of zero size) still lays its children out in a line, and inline
        // boxes keep their inline children: the table-row generated around their cells is wrapped in an inline-level
        // inline-table next.
        if !matches!(kind, FfiAnonymousTableBoxKind::InlineTable)
            && node_facts::kind_is_box(self.data(parent).kind.get())
        {
            let mut has_inline_child = false;
            let mut child = self.first_child(parent);
            while !child.is_invalid() {
                if child != wrapper_slot && node_is_inline_outside(self, child) {
                    has_inline_child = true;
                    break;
                }
                child = self.next_sibling(child);
            }
            if !has_inline_child {
                self.set_children_are_inline(parent, false);
            }
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
    if node_facts::has_flag(host.data(node), NodeFlag::ChildrenAreInline) {
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
        && node_facts::has_flag(child_data, NodeFlag::HasStyle)
        && !node_facts::has_flag(child_data, NodeFlag::Anonymous)
        && node_is_out_of_flow(host, child)
        && !node_has_replaced_element_table_display_adjustment(host, child)
        && is_table_non_root_box_with_display(host.display_before_box_type_transformation(child))
}

fn create_anonymous_wrapper(host: &mut TreeBuilderHost<'_>, parent: LayoutNode) -> LayoutNode {
    let wrapper = host.create_anonymous_wrapper_box(parent);
    let wrapper_slot = wrapper.slot();
    host.attach_child(parent, wrapper, NodeSlotId::INVALID);
    wrapper_slot
}

fn last_child_creating_anonymous_wrapper_if_needed(host: &mut TreeBuilderHost<'_>, parent: LayoutNode) -> LayoutNode {
    let last_child = host.last_child(parent);
    if last_child.is_invalid() {
        return create_anonymous_wrapper(host, parent);
    }
    let data = host.data(last_child);
    if !node_facts::has_flag(data, NodeFlag::Anonymous)
        || !node_facts::has_flag(data, NodeFlag::ChildrenAreInline)
        || node_is_generated_for_pseudo_element(data)
    {
        return create_anonymous_wrapper(host, parent);
    }
    last_child
}

// The insertion_parent_for_*() functions maintain the invariant that the in-flow children of
// block-level boxes must be either all block-level or all inline-level.
fn insertion_parent_for_inline_node(host: &mut TreeBuilderHost<'_>, parent: LayoutNode) -> LayoutNode {
    let data = host.data(parent);
    if matches!(data.kind.get(), NodeKind::FieldSetBox | NodeKind::SVGForeignObjectBox) {
        return last_child_creating_anonymous_wrapper_if_needed(host, parent);
    }

    // SVG layout ignores the inline/block distinction, and an anonymous wrapper would only hide
    // the child from SVGFormattingContext (e.g. a shape with a foreignObject sibling).
    if node_facts::kind_is_svg_box(data.kind.get()) || data.kind.get() == NodeKind::SVGSVGBox {
        return parent;
    }

    let parent_display = host.style(parent).map(|style| style.display());
    if node_is_inline_outside(host, parent) && parent_display.is_some_and(|display| display.is_flow_inside()) {
        return parent;
    }

    if parent_display.is_some_and(|display| display.is_flex_inside() || display.is_grid_inside()) {
        return last_child_creating_anonymous_wrapper_if_needed(host, parent);
    }

    if !has_in_flow_block_children(host, parent) || node_facts::has_flag(data, NodeFlag::ChildrenAreInline) {
        return parent;
    }

    // Parent has block-level children, insert into an anonymous wrapper block (and create it first if needed)
    last_child_creating_anonymous_wrapper_if_needed(host, parent)
}

fn nearest_rebuildable_container(host: &TreeBuilderHost<'_>, node: LayoutNode) -> LayoutNode {
    let mut container = node;
    loop {
        let data = host.data(container);
        if !node_facts::has_flag(data, NodeFlag::Anonymous) && data.kind.get() != NodeKind::InlineNode {
            return container;
        }
        container = host.parent(container);
        assert!(!container.is_invalid());
    }
}

fn insertion_parent_for_block_node(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    parent: LayoutNode,
    node: LayoutNode,
    mode: FfiInsertionMode,
) -> LayoutNode {
    let parent_data = host.data(parent);

    // Inline is fine for in-flow block children (interrupting blocks) and for out-of-flow children;
    // the inline formatting context emits items for both.
    // Block-level pseudo-element boxes climb out of inline ancestors instead (see below). A table-internal
    // pseudo-element box has to stay a child of its originating inline box though: table fixup generates one anonymous
    // inline-table around the consecutive table-internal children of the inline, so e.g. a `display: table-cell`
    // ::after joins the table of the table-cell siblings in the inline instead of starting a separate block-level
    // table outside of it.
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    let is_table_internal_pseudo_element_box = node_facts::has_flag(host.data(node), NodeFlag::Anonymous)
        && is_table_non_root_box_with_display(display_for_table_fixup(host, node));
    if (!node_facts::has_flag(host.data(node), NodeFlag::Anonymous) || is_table_internal_pseudo_element_box)
        && node_is_inline_outside(host, parent)
        && host.style(parent).is_some_and(|style| style.display().is_flow_inside())
    {
        return parent;
    }

    // SVG host ignores the inline/block distinction; wrapping existing inline-level siblings
    // (e.g. shapes next to a foreignObject) would only hide them from SVGFormattingContext.
    if node_facts::kind_is_svg_box(parent_data.kind.get()) || parent_data.kind.get() == NodeKind::SVGSVGBox {
        return parent;
    }

    // Make sure we're not inserting into an inline node, since those do not support block nodes.
    let mut new_parent = parent;
    while host.data(new_parent).kind.get() == NodeKind::InlineNode {
        new_parent = host.parent(new_parent);
        assert!(!new_parent.is_invalid());
    }

    if new_parent != parent && !is_inclusive_layout_ancestor_of(host, state.new_subtree_root, new_parent) {
        let container = nearest_rebuildable_container(host, new_parent);
        let element = host
            .arena()
            .commit_message_style_node(container)
            .and_then(StyleNodeID::from_raw)
            .filter(|style_node| style_node.element_index().is_some());
        if !state.layout_tree_rebuild_requests.contains(&element) {
            state.layout_tree_rebuild_requests.push(element);
        }
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

    let new_parent_data = host.data(new_parent);

    // If the block is out-of-flow,
    if node_is_out_of_flow(host, node) {
        let last_child = host.last_child(new_parent);
        assert!(!last_child.is_invalid());
        let last_child_data = host.data(last_child);

        // And we're appending while the parent's last child is an anonymous block, join that
        // anonymous block. Prepended boxes (e.g. an absolutely positioned ::before) belong at the
        // very start of the parent, not at the start of its trailing inline run.
        let new_parent_display = host.style(new_parent).map(|style| style.display());
        if mode == FfiInsertionMode::Append
            && !new_parent_display.is_some_and(|display| display.is_flex_inside() || display.is_grid_inside())
            && !node_is_generated_for_pseudo_element(last_child_data)
            && node_facts::has_flag(last_child_data, NodeFlag::Anonymous)
            && node_facts::has_flag(last_child_data, NodeFlag::ChildrenAreInline)
        {
            return last_child;
        }

        // Otherwise, insert this block into parent.
        return new_parent;
    }

    // If the parent block has block-level children, insert this block into parent.
    if !node_facts::has_flag(new_parent_data, NodeFlag::ChildrenAreInline) {
        return new_parent;
    }

    // Parent block has inline-level children (our siblings); wrap these siblings into an anonymous wrapper block.
    note_layout_tree_restructuring_at(host, state, new_parent);
    let mut children_to_wrap = Vec::new();
    let mut child = host.first_child(new_parent);
    while !child.is_invalid() {
        if !is_out_of_flow_table_internal_child_of_table_root(host, new_parent, child) {
            children_to_wrap.push(child);
        }
        child = host.next_sibling(child);
    }
    let wrapper = host.create_anonymous_wrapper_box(new_parent);
    let wrapper_slot = wrapper.slot();
    host.set_children_are_inline(wrapper_slot, true);
    for child in children_to_wrap {
        host.move_child(child, wrapper_slot, NodeSlotId::INVALID);
    }
    host.set_children_are_inline(new_parent, false);
    host.attach_child(new_parent, wrapper, NodeSlotId::INVALID);

    // Then it's safe to insert this block into parent.
    new_parent
}

fn insert_child_in_dom_order(
    host: &TreeBuilderHost<'_>,
    parent: LayoutNode,
    child: UnplacedLayoutNode,
    identity: StyleNodeID,
) {
    // An inline child of a block container with block children is placed in a newly appended
    // anonymous wrapper. Move that empty wrapper to the child's DOM position before filling it.
    let (parent_is_empty_anonymous_wrapper, wrapper_parent) = {
        let data = host.data(parent);
        (
            node_facts::has_flag(data, NodeFlag::Anonymous) && data.first_child.get().is_invalid(),
            data.parent.get(),
        )
    };
    if parent_is_empty_anonymous_wrapper && !wrapper_parent.is_invalid() {
        let mut sibling = host.next_dom_sibling(identity);
        while let Some(current) = sibling {
            let mut sibling_layout_node = host.arena().bound_row(current);
            while !sibling_layout_node.is_invalid() && host.parent(sibling_layout_node) != wrapper_parent {
                sibling_layout_node = host.parent(sibling_layout_node);
            }
            if !sibling_layout_node.is_invalid() && sibling_layout_node != parent {
                host.move_child(parent, wrapper_parent, sibling_layout_node);
                break;
            }
            sibling = host.next_dom_sibling(current);
        }
    }

    let mut sibling = host.next_dom_sibling(identity);
    while let Some(current) = sibling {
        let sibling_layout_node = host.arena().bound_row(current);
        if !sibling_layout_node.is_invalid() && host.parent(sibling_layout_node) == parent {
            host.attach_child(parent, child, sibling_layout_node);
            return;
        }
        sibling = host.next_dom_sibling(current);
    }

    let after_layout_node = pseudo_element_box_of_element_box(host, parent, GENERATED_FOR_AFTER);
    if !after_layout_node.is_invalid() {
        let mut after_layout_child = after_layout_node;
        while !host.parent(after_layout_child).is_invalid() && host.parent(after_layout_child) != parent {
            after_layout_child = host.parent(after_layout_child);
        }
        if host.parent(after_layout_child) == parent {
            host.attach_child(parent, child, after_layout_child);
            return;
        }
    }

    let mut layout_child = host.first_child(parent);
    while !layout_child.is_invalid() {
        if host.data(layout_child).generated_for.get() == GENERATED_FOR_AFTER {
            host.attach_child(parent, child, layout_child);
            return;
        }
        layout_child = host.next_sibling(layout_child);
    }
    host.attach_child(parent, child, NodeSlotId::INVALID);
}

fn insert_node_into_inline_or_block_ancestor(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    nearest_insertion_ancestor: LayoutNode,
    node: UnplacedLayoutNode,
    is_inline_outside: bool,
    mode: FfiInsertionMode,
    identity: Option<StyleNodeID>,
) {
    assert!(!nearest_insertion_ancestor.is_invalid());
    let node_slot = node.slot();

    let insertion_point = if is_inline_outside {
        insertion_parent_for_inline_node(host, nearest_insertion_ancestor)
    } else {
        insertion_parent_for_block_node(host, state, nearest_insertion_ancestor, node_slot, mode)
    };
    host.arena().note_inline_box_lifted_out_of(
        node_slot,
        (insertion_point != nearest_insertion_ancestor
            && host.data(nearest_insertion_ancestor).kind.get() == NodeKind::InlineNode)
            .then_some(nearest_insertion_ancestor),
    );

    // Insertion parents can be above the subtree being rebuilt in place: inline ancestors are
    // skipped, and out-of-flow boxes can join a trailing anonymous sibling. InDomOrder is only
    // selected after proving that an inline box can be added directly to a retained parent, so
    // that parent insertion is the planned update rather than an escape from its new subtree.
    if mode != FfiInsertionMode::InDomOrder {
        note_layout_tree_restructuring_at(host, state, insertion_point);
    }
    match mode {
        FfiInsertionMode::Append => host.attach_child(insertion_point, node, NodeSlotId::INVALID),
        FfiInsertionMode::Prepend => {
            let first_child = host.first_child(insertion_point);
            host.attach_child(insertion_point, node, first_child);
        }
        FfiInsertionMode::InDomOrder => insert_child_in_dom_order(
            host,
            insertion_point,
            node,
            identity.expect("an insertion in DOM order names its node"),
        ),
    }

    if is_inline_outside {
        // After inserting an inline-level box into a parent, mark the parent as having inline children.
        host.set_children_are_inline(insertion_point, true);
    } else if !node_is_out_of_flow(host, node_slot) {
        // Inline-flow parents keep their inline children flag; their IFC may contain interrupting blocks.
        if !node_is_inline_outside(host, insertion_point)
            || !host
                .style(insertion_point)
                .is_some_and(|style| style.display().is_flow_inside())
        {
            host.set_children_are_inline(insertion_point, false);
        }
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
            return FirstLetterTarget::not_found();
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
        host.arena().stamp_dom_row(first_letter_slice, NodeKind::TextNode, text);
        host.arena()
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
    if let Some(text) = host.arena().node_style_node(first_letter_slice) {
        for slice in [first_letter_slice, remainder_slice] {
            host.stamp_text_row_facts(slice, text);
        }
    }
    let first_letter_slice_slot = first_letter_slice;
    let remainder_slice_slot = remainder_slice;
    let first_letter_slice = host.created(first_letter_slice);
    let remainder_slice = host.created(remainder_slice);

    let wrapper_kind = host
        .arena()
        .with_style_store(|engine| {
            engine
                .pseudo_published_style_view(element, pseudo_kind_of(GENERATED_FOR_FIRST_LETTER))
                .map(|view| view.display())
        })
        .and_then(node_kind_for_display);
    let Some(wrapper_kind) = wrapper_kind else {
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
    host.arena()
        .stamp_pseudo_element_row(wrapper_slot, wrapper_kind, element, GENERATED_FOR_FIRST_LETTER);
    host.note_style_of_built_row(wrapper_slot, None);
    host.arena().owe_image_resources(
        wrapper_slot,
        OwedImageResources::StyleResources {
            owns_content_replacement_image: false,
        },
    );
    host.arena()
        .clear_pseudo_element_box(element, GENERATED_FOR_FIRST_LETTER);
    host.arena()
        .stamp_pseudo_element_box(wrapper_slot, element, GENERATED_FOR_FIRST_LETTER);
    let wrapper = host.created(wrapper_slot);
    let parent = host.parent(text_node);
    assert!(!parent.is_invalid());
    host.set_children_are_inline(wrapper_slot, true);
    host.attach_child(wrapper_slot, first_letter_slice, NodeSlotId::INVALID);
    host.attach_child(parent, wrapper, text_node);
    host.attach_child(parent, remainder_slice, text_node);
    host.arena().detach_child(parent, text_node);
    host.free_subtree(text_node);
}

fn is_marker_content(data: &NodeData) -> bool {
    data.kind.get() == NodeKind::ListItemMarkerBox || data.generated_for.get() == GENERATED_FOR_MARKER
}

// https://drafts.csswg.org/css-pseudo-4/#first-letter-application
fn find_first_letter_in_block(host: &mut TreeBuilderHost<'_>, block: LayoutNode) -> FirstLetterTarget {
    // NB: This walks a block container's inline descendants looking for the first-letter text. If the block has block
    //     children instead of inline, recurses into each in-flow block child in turn.
    if node_facts::has_flag(host.data(block), NodeFlag::ChildrenAreInline) {
        let mut result = FirstLetterTarget::not_found();
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
                result = find_first_letter_in_layout_text(host, node);
                return if result.found {
                    TraversalDecision::Break
                } else {
                    TraversalDecision::Continue
                };
            }
            if node_is_fragmented_inline(host, node) {
                return TraversalDecision::Continue;
            }
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
        let target = find_first_letter_in_block(host, child);
        if target.found {
            return target;
        }
        if !is_anonymous {
            break;
        }
        child = host.next_sibling(child);
    }
    FirstLetterTarget::not_found()
}

fn wrap_button_contents_if_needed(host: &mut TreeBuilderHost<'_>, layout_node: LayoutNode) {
    assert!(!layout_node.is_invalid());
    if !node_facts::has_flag(host.data(layout_node), NodeFlag::UsesButtonLayout) {
        return;
    }

    // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
    // If the element is an input element, or if it is a button element and its computed value for 'display' is not
    // 'inline-grid', 'grid', 'inline-flex', or 'flex', then the element's box has a child anonymous button content
    // box with the following behaviors:
    let display = host.style(layout_node).map(|style| style.display());
    if !display.is_some_and(|display| display.is_grid_inside() || display.is_flex_inside()) {
        let children_are_inline = node_facts::has_flag(host.data(layout_node), NodeFlag::ChildrenAreInline);
        let mut children = Vec::new();
        let mut child = host.first_child(layout_node);
        while !child.is_invalid() {
            children.push(child);
            child = host.next_sibling(child);
        }

        let flex_wrapper = host.create_anonymous_box(
            layout_node,
            AnonymousStyleKind::ButtonFlexWrapper,
            AnonymousStyleOverrides::default(),
            NodeKind::BlockContainer,
        );
        let content_box = host.create_anonymous_box(
            layout_node,
            AnonymousStyleKind::ButtonContentBox,
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

fn wrap_fieldset_contents_if_needed(host: &mut TreeBuilderHost<'_>, layout_node: LayoutNode) {
    assert!(!layout_node.is_invalid());

    // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
    // The anonymous fieldset content box is expected to appear after the rendered legend and is expected to contain
    // the content (including the '::before' and '::after' pseudo-elements) of the fieldset element except for the
    // rendered legend, if there is one.
    if host.data(layout_node).kind.get() == NodeKind::FieldSetBox {
        let legend = rendered_legend(host, layout_node);
        if legend.is_invalid() && !host.display(layout_node).is_flex_inside() {
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

        let style = node_facts::node_style_view(host.data(layout_node)).expect("fieldset style");
        let overrides = AnonymousStyleOverrides {
            inline_block_wrapper: false,
            overflow_x: style.box_values().overflow_x,
            overflow_y: style.box_values().overflow_y,
        };
        host.arena()
            .update_layout_style(host.host_calls(), layout_node, |style| {
                style.set_overflow(
                    crate::css::css_enums::overflow::VISIBLE,
                    crate::css::css_enums::overflow::VISIBLE,
                );
            });
        let wrapper = host.create_anonymous_box(
            layout_node,
            AnonymousStyleKind::FieldsetContentWrapper,
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
        || node_facts::has_flag(host.data(node), NodeFlag::Anonymous)
        || node_facts::has_flag(host.data(node), NodeFlag::UsesButtonLayout)
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

fn is_table_non_root_box_sibling(host: &TreeBuilderHost<'_>, sibling: LayoutNode) -> bool {
    // Text nodes carry their parent's style, so only boxes can be table-non-root boxes.
    !sibling.is_invalid()
        && node_facts::kind_is_box(host.data(sibling).kind.get())
        && is_table_non_root_box(host, sibling)
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

fn text_is_ascii_whitespace(host: &mut TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    super::rendered_text::ensure_text_content(host.arena, node);
    host.arena()
        .text_content(node)
        .expect("text was just refreshed")
        .text
        .iter()
        .all(|unit| matches!(unit, 0x09..=0x0d | 0x20))
}

fn is_ignorable_whitespace(host: &mut TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    if node_facts::kind_is_text(host.data(node).kind.get()) && text_is_ascii_whitespace(host, node) {
        return true;
    }

    let data = host.data(node);
    if node_facts::has_flag(data, NodeFlag::Anonymous)
        && node_facts::kind_is_block_container(data.kind.get())
        && node_facts::has_flag(data, NodeFlag::ChildrenAreInline)
    {
        let mut contains_only_whitespace = true;
        host.for_each_in_inclusive_subtree(node, |host, descendant| {
            let descendant_data = host.data(descendant);
            if node_facts::kind_is_text(descendant_data.kind.get()) {
                if !text_is_ascii_whitespace(host, descendant) {
                    contains_only_whitespace = false;
                    return TraversalDecision::Break;
                }
            } else if node_is_out_of_flow(host, descendant)
                || !node_facts::has_flag(descendant_data, NodeFlag::Anonymous)
            {
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
    host: &mut TreeBuilderHost<'_>,
    parent: LayoutNode,
    matcher: impl Fn(&TreeBuilderHost<'_>, LayoutNode) -> bool,
    mut callback: impl FnMut(&mut TreeBuilderHost<'_>, &[LayoutNode], LayoutNode),
) {
    let mut sequence: Vec<LayoutNode> = Vec::new();
    let mut end_sequence = |host: &mut TreeBuilderHost<'_>, sequence: &mut Vec<LayoutNode>, mut nearest_sibling| {
        // Whitespace that follows the last matching child is not part of the sequence. The fixup algorithm only
        // discards whitespace-only boxes that lie between two table-non-root boxes (step 1), so whitespace after the
        // last box of the sequence stays outside the anonymous wrapper: in "a <cell>b</cell><cell>c</cell> d" the
        // space before "d" is ordinary inline content next to the generated inline-table.
        while sequence.last().is_some_and(|&last| !matcher(host, last)) {
            nearest_sibling = sequence.pop().expect("a trailing whitespace node");
        }
        if !sequence.iter().all(|&node| is_ignorable_whitespace(host, node)) {
            callback(host, sequence, nearest_sibling);
        }
        sequence.clear();
    };
    let mut child = host.first_child(parent);
    while !child.is_invalid() {
        if matcher(host, child) || (!sequence.is_empty() && is_ignorable_whitespace(host, child)) {
            sequence.push(child);
        } else if !sequence.is_empty() {
            end_sequence(host, &mut sequence, child);
        }
        child = host.next_sibling(child);
    }
    if !sequence.is_empty() {
        end_sequence(host, &mut sequence, NodeSlotId::INVALID);
    }
}

fn remove_irrelevant_boxes(host: &mut TreeBuilderHost<'_>, root: LayoutNode) {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 1. Remove irrelevant boxes:
    // The following boxes are discarded as if they were display:none:
    let mut to_remove = Vec::new();
    host.for_each_in_inclusive_subtree(root, |host, node| {
        let is_box = node_facts::kind_is_box(host.data(node).kind.get());

        // 1. Children of a table-column.
        if is_box && host.display(node).is_table_column() {
            host.set_children_are_inline(node, false);
            let mut child = host.first_child(node);
            while !child.is_invalid() {
                to_remove.push(child);
                child = host.next_sibling(child);
            }
        }

        // 2. Children of a table-column-group which are not a table-column.
        if is_box && host.display(node).is_table_column_group() {
            host.set_children_are_inline(node, false);
            let mut child = host.first_child(node);
            while !child.is_invalid() {
                if !host.display(child).is_table_column() {
                    to_remove.push(child);
                }
                child = host.next_sibling(child);
            }
        }

        // Steps 1 and 2 already scheduled the children of table-column boxes and the non-column children of
        // table-column-group boxes when visiting their parent; their whole subtree goes away with them.
        let parent = host.parent(node);
        if !parent.is_invalid() && node_facts::kind_is_box(host.data(parent).kind.get()) {
            let parent_display = host.display(parent);
            if parent_display.is_table_column()
                || (parent_display.is_table_column_group() && !host.display(node).is_table_column())
            {
                return TraversalDecision::SkipChildrenAndContinue;
            }
        }

        // 3. Anonymous inline boxes which contain only white space and are between two immediate siblings each of
        //    which is a table-non-root box.
        // This is what keeps "<cell>b</cell> <cell>c</cell>" a single table with adjacent cells, regardless of whether
        // the siblings live in a table, in a block (where the whitespace sits in an anonymous block wrapper) or in an
        // inline box. The whitespace before the first and after the last table-non-root box of such a run is not
        // discarded and remains ordinary inline content.
        if !parent.is_invalid()
            && is_table_non_root_box_sibling(host, host.previous_sibling(node))
            && is_table_non_root_box_sibling(host, host.next_sibling(node))
            && is_ignorable_whitespace(host, node)
        {
            to_remove.push(node);
            return TraversalDecision::SkipChildrenAndContinue;
        }

        // 4. Anonymous inline boxes which meet all of the following criteria:
        //    - they contain only white space
        //    - they are the first and/or last child of a tabular container
        //    - whose immediate sibling, if any, is a table-non-root box
        if is_box
            && !parent.is_invalid()
            && is_tabular_container(host, parent)
            && !node_facts::has_flag(host.data(parent), NodeFlag::Anonymous)
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

fn generate_missing_child_wrappers(host: &mut TreeBuilderHost<'_>, root: LayoutNode) {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 2. Generate missing child wrappers:
    host.for_each_in_inclusive_subtree(root, |host, parent| {
        let data = host.data(parent);
        if !node_facts::kind_is_box(data.kind.get()) {
            return TraversalDecision::Continue;
        }
        // AD-HOC: SVG layout derives box types from the element, so display values must not introduce anonymous boxes
        //         inside SVG content.
        if node_facts::kind_is_svg_box(data.kind.get()) || data.kind.get() == NodeKind::SVGSVGBox {
            return TraversalDecision::Continue;
        }

        let display = host.display(parent);
        if display.is_table_inside() {
            // 1. An anonymous table-row box must be generated around each sequence of consecutive children of a
            //    table-root box which are not proper table child boxes.
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |host, child| {
                    !node_facts::has_flag(host.data(child), NodeFlag::HasStyle) || !is_proper_table_child(host, child)
                },
                |host, sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableRow);
                },
            );
        } else if display.is_table_row_group() || display.is_table_header_group() || display.is_table_footer_group() {
            // 2. An anonymous table-row box must be generated around each sequence of consecutive children of a
            //    table-row-group box which are not table-row boxes.
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |host, child| {
                    !node_facts::has_flag(host.data(child), NodeFlag::HasStyle) || !host.display(child).is_table_row()
                },
                |host, sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableRow);
                },
            );
        } else if display.is_table_row() {
            // 3. An anonymous table-cell box must be generated around each sequence of consecutive children of a
            //    table-row box which are not table-cell boxes.
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |host, child| {
                    !node_facts::has_flag(host.data(child), NodeFlag::HasStyle) || !host.display(child).is_table_cell()
                },
                |host, sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableCell);
                },
            );
        }
        TraversalDecision::Continue
    });
}

fn generate_missing_parents(host: &mut TreeBuilderHost<'_>, root: LayoutNode) -> Vec<LayoutNode> {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 3. Generate missing parents:
    let mut table_roots_to_wrap = Vec::new();
    host.for_each_in_inclusive_subtree(root, |host, parent| {
        let (has_style, is_box, kind) = {
            let data = host.data(parent);
            (
                node_facts::has_flag(data, NodeFlag::HasStyle),
                node_facts::kind_is_box(data.kind.get()),
                data.kind.get(),
            )
        };
        let current_display = host.display(parent);
        let is_inline_outside = node_is_inline_outside(host, parent);
        if !has_style {
            return TraversalDecision::Continue;
        }
        let node_is_svg_content = node_facts::kind_is_svg_box(kind) || kind == NodeKind::SVGSVGBox;

        // 1. An anonymous table-row box must be generated around each sequence of consecutive table-cell boxes whose
        //    parent is not a table-row.
        if !node_is_svg_content && !current_display.is_table_row() {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |host, child| {
                    node_facts::has_flag(host.data(child), NodeFlag::HasStyle) && host.display(child).is_table_cell()
                },
                |host, sequence, nearest_sibling| {
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

        // A table-row is misparented if its parent is neither a table-row-group nor a table-root box.
        // A table-column box is misparented if its parent is neither a table-column-group box nor a table-root box.
        // A table-row-group, table-column-group, or table-caption box is misparented if its parent is not a table-root
        // box.
        // The sequence spans misparented proper table children of every kind: the anonymous table-row generated
        // around loose cells by step 1 and the table-row-group next to it belong to the same anonymous table.
        if !node_is_svg_content && !current_display.is_table_inside() {
            let is_table_row_group = current_display.is_table_row_group_kind();
            let is_table_column_group = current_display.is_table_column_group();
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |host, child| {
                    if !node_facts::has_flag(host.data(child), NodeFlag::HasStyle) {
                        return false;
                    }
                    let display = host.display(child);
                    if display.is_table_row() {
                        return !is_table_row_group;
                    }
                    if display.is_table_column() {
                        return !is_table_column_group;
                    }
                    let display = display_for_table_fixup(host, child);
                    is_table_track_group(display) || display.is_table_caption()
                },
                |host, sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, anonymous_table_kind);
                },
            );
        }

        // 3. An anonymous table-wrapper box must be generated around each table-root.
        if is_box && current_display.is_table_inside() {
            let wrap_parent = host.parent(parent);
            let wrap_parent_is_svg_content = !wrap_parent.is_invalid() && {
                let wrap_parent_kind = host.data(wrap_parent).kind.get();
                node_facts::kind_is_svg_box(wrap_parent_kind) || wrap_parent_kind == NodeKind::SVGSVGBox
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
                AnonymousStyleKind::TableWrapper,
                AnonymousStyleOverrides::default(),
                NodeKind::TableWrapper,
            );
            host.arena()
                .reset_table_box_style_used_by_wrapper(host.host_calls(), table_root);
            let wrapper_slot = wrapper.slot();
            host.move_child(table_root, wrapper_slot, NodeSlotId::INVALID);
            host.attach_child(parent, wrapper, nearest_sibling);
        }
    }
    table_roots_to_wrap
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
        host.arena()
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
    if node_facts::kind_is_text(root_data.kind.get()) || !node_facts::has_flag(root_data, NodeFlag::HasStyle) {
        let mut ancestor = parent;
        while !ancestor.is_invalid() {
            let ancestor_data = host.data(ancestor);
            let ancestor_display = display_for_table_fixup(host, ancestor);
            if is_tabular_container(host, ancestor) || ancestor_display.is_table_column_group() {
                return ancestor;
            }
            if !node_facts::has_flag(ancestor_data, NodeFlag::Anonymous) {
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
        if node_facts::has_flag(data, NodeFlag::HasStyle) && display_for_table_fixup(host, current).is_table_inside() {
            return Some(current);
        }
        current = host.parent(current);
    }
    None
}

fn fixup_tables_in_rebuilt_subtrees(
    host: &mut TreeBuilderHost<'_>,
    rebuilt_subtree_roots: &[LayoutNode],
    reused_child_list_update_roots: &[LayoutNode],
    additional_roots: &[LayoutNode],
) {
    // A build can rebuild thousands of subtrees, one per element whose text was replaced, so the roots are kept unique,
    // and found inside one another, through sets rather than by comparing every pair.
    let mut roots = Vec::new();
    let mut root_set = HashSet::default();
    for &root in rebuilt_subtree_roots {
        if host.arena().node_data_if_live(root).is_none() || host.parent(root).is_invalid() {
            continue;
        }
        let scope = table_fixup_scope_for_rebuilt_subtree(host, root);
        if root_set.insert(scope) {
            roots.push(scope);
        }
    }
    if !reused_child_list_update_roots.is_empty() {
        // The inclusive ancestors of the live rebuilt subtree roots. A walk stops at an ancestor another walk reached,
        // whose ancestors that walk reached too.
        let mut ancestors_of_rebuilt_roots = HashSet::default();
        for &rebuilt_root in rebuilt_subtree_roots {
            if host.arena().node_data_if_live(rebuilt_root).is_none() {
                continue;
            }
            let mut current = rebuilt_root;
            while !current.is_invalid() && ancestors_of_rebuilt_roots.insert(current) {
                current = host.parent(current);
            }
        }
        for &root in reused_child_list_update_roots {
            if host.arena().node_data_if_live(root).is_none() || host.parent(root).is_invalid() {
                continue;
            }
            if !ancestors_of_rebuilt_roots.contains(&root) && root_set.insert(root) {
                roots.push(root);
            }
        }
    }
    for &root in additional_roots {
        if host.arena().node_data_if_live(root).is_none() || host.parent(root).is_invalid() {
            continue;
        }
        if root_set.insert(root) {
            roots.push(root);
        }
    }

    // A root inside another root's subtree is fixed up with it.
    roots.retain(|&candidate| {
        let mut ancestor = host.parent(candidate);
        while !ancestor.is_invalid() {
            if root_set.contains(&ancestor) {
                return false;
            }
            ancestor = host.parent(ancestor);
        }
        true
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

fn fixup_tables(host: &mut TreeBuilderHost<'_>, root: LayoutNode) {
    assert!(!root.is_invalid());
    remove_irrelevant_boxes(host, root);
    generate_missing_child_wrappers(host, root);
    let table_roots = generate_missing_parents(host, root);
    missing_cells_fixup(host, &table_roots);
}

#[cfg(test)]
mod tests {
    use crate::css::style::bridge::element_adjustment_fact;
    use crate::layout::node_data::NodeSlotId;
    use crate::layout::tree_builder::{
        ComputedContentType, FfiCodePointCategoryFacts, FfiElementLayoutKind, FfiPrincipalBoxPlacement,
        FfiPseudoElement, FfiPseudoElementDecision, FfiReplacedElementDisplayAdjustment, LayoutNodeReuse,
        PrincipalBoxGenerationDecision, PrincipalBoxPlacementFacts, PrincipalNodeEntryFacts, PrincipalNodeKind,
        PseudoElementFacts, SvgEntryDecision, TopLayerEntryDecision, TreeBuilderContext,
        adjusted_table_display_for_replaced_element, display_contents_text_needs_style_wrapper, element_layout_kind,
        find_first_letter_in_text, principal_box_generation_decision, principal_box_placement_decision,
        principal_node_entry_decision, pseudo_element_decision,
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
        work.resolve(&arena).pay(&main_thread);

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

    #[test]
    fn an_unstyled_element_with_an_existing_box_requests_style_and_discards_the_box() {
        use crate::css::style::tree::StyleNodeID;
        use crate::layout::LayoutNodeArena;
        use crate::layout::node_data::{NodeConstructionFacts, NodeKind};
        use crate::stage::MainThread;

        let mut engine = crate::css::style::StyleEngine::new();
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let mut raw = [0_u32];
        engine.allocate_style_nodes(&mut raw);
        let element = StyleNodeID::from_raw(raw[0]).unwrap();
        let facts = NodeConstructionFacts {
            kind: NodeKind::BlockContainer,
            is_anonymous: false,
            is_html_input_element: false,
            is_html_html_element: false,
            is_document_element: false,
            is_in_user_agent_shadow_tree: false,
            uses_button_layout: false,
            is_editing_host: false,
            is_body: false,
            dom_paint_facts: 0,
            style_node: element.raw(),
        };
        let element_box = arena.allocate(facts);
        let mut parent_facts = facts;
        parent_facts.is_anonymous = true;
        parent_facts.style_node = 0;
        let parent = arena.allocate(parent_facts);
        arena.attach_child(parent, super::UnplacedLayoutNode::new(element_box), NodeSlotId::INVALID);
        arena.bind_row(element_box);
        arena.queue_box_presence();
        let work = crate::layout::tree_mutation::OwedHostWork::default();
        let mut host = super::TreeBuilderHost {
            arena: &mut arena,
            work: &work,
        };
        let mut state = super::TreeBuilderState::default();
        let mut context = TreeBuilderContext::default();
        let mut update = super::PrincipalNodeUpdate {
            kind: PrincipalNodeKind::Element,
            reuse: LayoutNodeReuse::default(),
            host: &mut host,
            state: &mut state,
            old_layout_node: element_box,
            identity: element,
            style_node: Some(element),
            element_type_facts: 0,
            context: &mut context,
            must_create_subtree: false,
            insertion_mode: super::FfiInsertionMode::Append,
        };
        let facts = PrincipalNodeEntryFacts {
            must_create_subtree: false,
            needs_layout_tree_update: false,
            has_layout_node: true,
            layout_node_is_attached: true,
        };
        let decision = principal_node_entry_decision(facts, update.reuse, update.kind, 0, update.context);
        assert!(!decision.should_create_layout_node);
        super::update_principal_node_after_entry(&mut update, facts, decision);
        work.resolve(&arena).pay(&MainThread::for_test());

        assert!(state.reached_unstyled_element);
        assert_eq!(state.reports.len(), 1);
        assert_eq!(state.reports[0].style_node, element.raw());
        assert!(state.reports[0].kind == crate::layout::commit::FfiCommitMessageKind::UnstyledElementReached);
        assert!(!arena.slot_is_live(element_box));
        assert!(arena.bound_row(element).is_invalid());
        assert!(arena.data(parent).first_child.get().is_invalid());
        arena
            .free_subtree(parent)
            .destroy_shells_and_invoke_callbacks(&MainThread::for_test());
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
                      originating_layout_node_is_list_item: bool,
                      normal_marker_has_content| {
            pseudo_element_decision(PseudoElementFacts {
                has_style: true,
                pseudo_element,
                content_type,
                display_is_none,
                display_is_contents,
                display_is_list_item,
                display_is_inline_flow: false,
                has_content_replacement,
                originating_list_box: if originating_layout_node_is_list_item {
                    NodeSlotId::new(1, 1)
                } else {
                    NodeSlotId::INVALID
                },
                normal_marker_has_content,
                marker_position_is_inside: false,
            })
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
            FfiPseudoElementDecision::None
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
            FfiPseudoElementDecision::Box
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
            FfiPseudoElementDecision::ContentReplacement
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
            FfiPseudoElementDecision::Contents
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
            FfiPseudoElementDecision::Box
        );
    }

    #[test]
    fn principal_node_entry_decisions() {
        let mut facts = PrincipalNodeEntryFacts {
            must_create_subtree: false,
            needs_layout_tree_update: false,
            has_layout_node: true,
            layout_node_is_attached: true,
        };
        let mut element_type_facts = 0;
        let mut context = TreeBuilderContext::default();
        let decision = principal_node_entry_decision(
            facts,
            LayoutNodeReuse::default(),
            PrincipalNodeKind::Element,
            element_type_facts,
            &context,
        );
        assert!(!decision.should_create_layout_node);
        assert_eq!(decision.top_layer, TopLayerEntryDecision::Continue);
        assert_eq!(decision.svg, SvgEntryDecision::Continue);

        facts.layout_node_is_attached = false;
        element_type_facts = element_adjustment_fact::RENDERED_IN_TOP_LAYER;
        let decision = principal_node_entry_decision(
            facts,
            LayoutNodeReuse::default(),
            PrincipalNodeKind::Element,
            element_type_facts,
            &context,
        );
        assert_eq!(decision.top_layer, TopLayerEntryDecision::SkipAndRequestZoneRebuild);

        element_type_facts = element_adjustment_fact::REQUIRES_SVG_CONTAINER;
        let decision = principal_node_entry_decision(
            facts,
            LayoutNodeReuse::default(),
            PrincipalNodeKind::Element,
            element_type_facts,
            &context,
        );
        assert_eq!(decision.svg, SvgEntryDecision::Skip);

        facts.must_create_subtree = true;
        element_type_facts |= element_adjustment_fact::IS_SVG_CONTAINER;
        context.has_svg_root = false;
        let decision = principal_node_entry_decision(
            facts,
            LayoutNodeReuse::default(),
            PrincipalNodeKind::Element,
            element_type_facts,
            &context,
        );
        assert!(decision.should_create_layout_node);
        assert_eq!(decision.svg, SvgEntryDecision::EnterSvgRoot);

        element_type_facts =
            element_adjustment_fact::REQUIRES_SVG_CONTAINER | element_adjustment_fact::IS_SVG_FOREIGN_OBJECT_ELEMENT;
        context.has_svg_root = true;
        let decision = principal_node_entry_decision(
            facts,
            LayoutNodeReuse::default(),
            PrincipalNodeKind::Element,
            element_type_facts,
            &context,
        );
        assert_eq!(decision.svg, SvgEntryDecision::EnterForeignContent);
        context.has_svg_root = false;
        let decision = principal_node_entry_decision(
            facts,
            LayoutNodeReuse::default(),
            PrincipalNodeKind::Element,
            element_type_facts,
            &context,
        );
        assert_eq!(decision.svg, SvgEntryDecision::Skip);

        element_type_facts = 0;
        context.has_svg_root = true;
        let decision = principal_node_entry_decision(
            facts,
            LayoutNodeReuse::default(),
            PrincipalNodeKind::Element,
            element_type_facts,
            &context,
        );
        assert_eq!(decision.svg, SvgEntryDecision::Skip);
        context.has_svg_root = false;
        let decision = principal_node_entry_decision(
            facts,
            LayoutNodeReuse::default(),
            PrincipalNodeKind::Element,
            element_type_facts,
            &context,
        );
        assert_eq!(decision.svg, SvgEntryDecision::Continue);
    }

    #[test]
    fn specialized_element_layout_kinds() {
        assert_eq!(
            element_layout_kind(false, 0, false, false),
            FfiElementLayoutKind::Normal
        );
        assert_eq!(
            element_layout_kind(true, 0, false, false),
            FfiElementLayoutKind::ContentReplacement
        );
        assert_eq!(
            element_layout_kind(false, element_adjustment_fact::IS_SVG_MASK_ELEMENT, true, false),
            FfiElementLayoutKind::SvgMask
        );
        assert_eq!(
            element_layout_kind(false, element_adjustment_fact::IS_SVG_PATTERN_ELEMENT, false, true),
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
