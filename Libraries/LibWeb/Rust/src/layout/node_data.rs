/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::tree_shape::ShapeCell;
use crate::layout::CssPixels;
use std::cell::Cell;
use std::ffi::c_void;

pub const GENERATED_FOR_AFTER: u8 = 1;
pub const GENERATED_FOR_BACKDROP: u8 = 2;
pub const GENERATED_FOR_BEFORE: u8 = 3;
pub const GENERATED_FOR_FIRST_LETTER: u8 = 4;
pub const GENERATED_FOR_MARKER: u8 = 6;
/// The last pseudo-element an element holds a box for in its own right; the ones from
/// `GENERATED_FOR_AFTER` up to it are an element's synthetic pseudo-elements.
pub const GENERATED_FOR_LAST_SYNTHETIC: u8 = 10;
/// `CSS::PseudoElement::Selection`, as the style engine numbers an element's pseudo-element
/// records. It generates no box, so no row names it.
pub const SELECTION_PSEUDO_KIND: u8 = 8;
/// `CSS::PseudoElement::SearchText`, as the style engine numbers an element's pseudo-element
/// records. It generates no box either.
pub const SEARCH_TEXT_PSEUDO_KIND: u8 = 6;
/// The current find-in-page match's `::search-text`, as the style engine numbers an element's
/// pseudo-element records. It generates no box either.
pub const SEARCH_TEXT_CURRENT_PSEUDO_KIND: u8 = 7;

/// The pseudo-element a row generated for `generated_for` stands for, as the style engine numbers
/// an element's pseudo-element records. `Layout::Node::encode_generated_for` is its inverse.
pub(crate) const fn pseudo_kind_of(generated_for: u8) -> u8 {
    generated_for - 1
}

// The full C++ StyleGroupIndex space; LayoutRustBridge.cpp static-asserts the
// count so the style container array and the registered group indices line up.
pub const STYLE_GROUP_COUNT: usize = 23;

/// Where a row's computed style is: the group payload array of the style record pinned for the
/// row, or null for a row with no style. The array and the groups it names are immutable, so the
/// row shares it as a `HostShared` rather than as a raw pointer.
pub(crate) type StylePayloadsRef = crate::css::host_shared::HostShared<FfiStylePayloads>;

/// The group payload array a row's style addresses, or `None` for a row with no style.
///
/// # Safety
///
/// The style record pinned for the row must outlive `'a`.
pub(crate) unsafe fn style_payloads<'a>(style: StylePayloadsRef) -> Option<&'a FfiStylePayloads> {
    // SAFETY: A non-null style pointer addresses the group pointer array of the style record, which
    // FfiStylePayloads mirrors exactly, and the caller keeps the record alive for 'a.
    (!style.is_null()).then(|| unsafe { style.deref() })
}

#[derive(Clone, Copy, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiReplacedContentFacts {
    pub has_auto_content_width: bool,
    pub auto_content_width: CssPixels,
    pub has_auto_content_height: bool,
    pub auto_content_height: CssPixels,
    pub auto_content_aspect_ratio_numerator: CssPixels,
    pub auto_content_aspect_ratio_denominator: CssPixels,
    pub has_default_preferred_width: bool,
    pub default_preferred_width: CssPixels,
    pub has_default_preferred_height: bool,
    pub default_preferred_height: CssPixels,
}

impl From<crate::painting::host::FfiNaturalSize> for crate::css::style::NaturalSize {
    fn from(size: crate::painting::host::FfiNaturalSize) -> Self {
        Self {
            width: size.width.has_value.then_some(size.width.value.raw_value()),
            height: size.height.has_value.then_some(size.height.value.raw_value()),
            aspect_ratio: size.has_aspect_ratio.then_some((
                size.aspect_ratio_numerator.raw_value(),
                size.aspect_ratio_denominator.raw_value(),
            )),
        }
    }
}

impl From<crate::css::style::NaturalSize> for crate::painting::host::FfiNaturalSize {
    fn from(size: crate::css::style::NaturalSize) -> Self {
        let (numerator, denominator) = size.aspect_ratio.unwrap_or_default();
        Self {
            width: size.width.map(CssPixels::from_raw).into(),
            height: size.height.map(CssPixels::from_raw).into(),
            has_aspect_ratio: size.aspect_ratio.is_some(),
            aspect_ratio_numerator: CssPixels::from_raw(numerator),
            aspect_ratio_denominator: CssPixels::from_raw(denominator),
        }
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStylePayloads {
    pub groups: [*const c_void; STYLE_GROUP_COUNT],
}

// SAFETY: A style record's payload array is written once, as the record is interned, and never
// through a shared reference, and the groups it points at are immutable while the record is pinned.
// Sharing one shares only reads.
unsafe impl Sync for FfiStylePayloads {}

impl Default for FfiStylePayloads {
    fn default() -> Self {
        Self {
            groups: [std::ptr::null(); STYLE_GROUP_COUNT],
        }
    }
}

pub use super::node_slot_id::{MAX_NODE_SLOT_COUNT, NodeSlotId};

/// A DOM node, named the way the host names one without pointing at it: the document, by itself,
/// or any other node by its StyleNodeID. No node at all is a StyleNodeID of 0.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiNodeIdentity {
    pub style_node: u32,
    pub is_document: bool,
}

impl FfiNodeIdentity {
    pub(crate) fn is_none(self) -> bool {
        self.style_node == 0 && !self.is_document
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum NodeKind {
    Unset = 0,
    AudioBox = 1,
    BlockContainer = 2,
    Box = 3,
    BreakNode = 4,
    CanvasBox = 5,
    CheckBox = 6,
    FieldSetBox = 7,
    GeneratedTextNode = 8,
    ImageBox = 9,
    InlineNode = 10,
    LegendBox = 11,
    ListItemBox = 12,
    ListItemMarkerBox = 13,
    NavigableContainerViewport = 14,
    Node = 15,
    RadioButton = 18,
    RangeInputBox = 19,
    SVGClipBox = 22,
    SVGForeignObjectBox = 23,
    SVGGeometryBox = 24,
    SVGGraphicsBox = 25,
    SVGImageBox = 26,
    SVGMaskBox = 27,
    SVGPatternBox = 28,
    SVGSVGBox = 29,
    SVGTextBox = 30,
    SVGTextPathBox = 31,
    TableWrapper = 32,
    TextAreaBox = 33,
    TextInputBox = 34,
    TextNode = 35,
    VideoBox = 37,
    Viewport = 38,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum NodeFlag {
    Anonymous = 1 << 0,
    HasStyle = 1 << 1,
    ChildrenAreInline = 1 << 2,
    IsFlexItem = 1 << 3,
    IsGridItem = 1 << 4,
    /// The box is an element's or a pseudo-element's own box, and that element or pseudo-element
    /// stores a scroll offset other than zero. The overflow update measures such a box eagerly
    /// after a full commit so the offset can be clamped.
    HasScrollOffset = 1 << 5,
    IsBody = 1 << 6,
    NeedsLayoutUpdate = 1 << 7,
    NeedsOwnGeometryUpdate = 1 << 8,
    AbsposDescendantEscapes = 1 << 9,
    CompensatesForHorizontalScroll = 1 << 10,
    CompensatesForVerticalScroll = 1 << 11,
    IsReplacedElement = 1 << 12,
    IsHtmlInputElement = 1 << 13,
    IsHtmlHtmlElement = 1 << 14,
    IsInUserAgentShadowTree = 1 << 15,
    UsesButtonLayout = 1 << 16,
    IsEditingHost = 1 << 17,
    ReplacedBoxCanHaveChildren = 1 << 18,
    IsPseudoElementPrincipalBox = 1 << 19,
    FollowsPrincipalStyle = 1 << 20,
    EstablishesAbsolutePositionContainingBlock = 1 << 21,
    ProducesLineBoxFragmentWhenEmpty = 1 << 22,
    ListMarkerIsInside = 1 << 23,
    HasAnchorNames = 1 << 24,
    InsetsUseAnchorFunctions = 1 << 25,
    HasCommittedFragmentLink = 1 << 26,
    HasPreserve3dTransformStyle = 1 << 27,
    IsMissingTableCell = 1 << 28,
    HasAnimatedOpacityOrTransform = 1 << 29,
    IsDocumentElement = 1 << 30,
    EstablishesFixedPositionContainingBlock = 0x8000_0000,
}

impl NodeFlag {
    /// The flags that say what node a row stands for. A row is built with them, and installing a style changes none.
    pub(crate) const IDENTITY: u32 = Self::Anonymous as u32 | Self::IsBody as u32 | Self::IsDocumentElement as u32;
}

/// Facts a node takes from its ancestors. They are derived when the node is attached or its
/// ancestors' styles change, so laying out a subtree never reads above it to learn them.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub(crate) enum AncestorFact {
    /// The parent's inner display type is flex or grid.
    ParentIsFlexOrGridContainer = 1 << 0,
    /// The parent is unstyled, or is not floating and has a flow or flow-root inner display type.
    ParentIsUnfloatedFlowContainer = 1 << 1,
    /// An anonymous box whose parent uses button layout, like the wrapper around a button's content.
    IsAnonymousButtonContentWrapper = 1 << 2,
    /// An anonymous box whose parent is an anonymous button content wrapper.
    IsAnonymousButtonContentBox = 1 << 3,
    /// The node or one of its ancestors has an inline outer display type.
    HasInlineLevelInclusiveAncestor = 1 << 4,
    /// An anonymous box whose nearest non-anonymous ancestor puts an ellipsis on overflowing lines.
    InheritsTextOverflowEllipsis = 1 << 5,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum CompositorAnimationFrameKind {
    Opacity = 1 << 0,
    BackgroundColor = 1 << 1,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
#[cfg_attr(not(test), expect(dead_code, reason = "C++ constructs the variants"))]
pub enum FfiNodeLink {
    Parent,
    FirstChild,
    LastChild,
    PreviousSibling,
    NextSibling,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum DomPaintFact {
    Inert = 1 << 0,
    EditableOrEditingHost = 1 << 1,
    InsideBlockingWheelEventHandler = 1 << 2,
    NestedNavigableContainer = 1 << 3,
}

#[cfg(test)]
#[derive(Clone, Copy)]
pub(crate) struct NodeConstructionFacts {
    pub kind: NodeKind,
    pub is_anonymous: bool,
    pub is_html_input_element: bool,
    pub is_html_html_element: bool,
    pub is_document_element: bool,
    pub is_in_user_agent_shadow_tree: bool,
    pub uses_button_layout: bool,
    pub is_editing_host: bool,
    pub is_body: bool,
    pub dom_paint_facts: u8,
    /// The StyleNodeID of the element the row is bound to, or 0.
    pub style_node: u32,
}

#[repr(C)]
#[derive(Clone)]
pub(crate) struct NodeData {
    pub parent: ShapeCell<NodeSlotId>,
    pub first_child: ShapeCell<NodeSlotId>,
    pub last_child: ShapeCell<NodeSlotId>,
    pub previous_sibling: ShapeCell<NodeSlotId>,
    pub next_sibling: ShapeCell<NodeSlotId>,
    pub kind: ShapeCell<NodeKind>,
    pub generated_for: ShapeCell<u8>,
    pub intrinsic_cache_epoch: Cell<u16>,
    pub flags: ShapeCell<u32>,
    /// Advanced on every layout invalidation that reaches this node or its
    /// subtree, with no propagation boundary: unlike the intrinsic epoch,
    /// changes inside absolutely positioned and SVG descendants must reach
    /// every ancestor, because their fragments live in ancestor run trees.
    /// Wide enough that wrapping between a cache store and the next probe
    /// is unreachable.
    pub fragment_cache_epoch: Cell<u32>,
    pub slot_generation: ShapeCell<u8>,
    pub compositor_animation_frame_kinds: ShapeCell<u8>,
    pub table_column_span: Cell<u16>,
    pub table_row_span: Cell<u16>,
    pub dom_paint_facts: ShapeCell<u8>,
    pub ancestor_facts: Cell<u8>,
    pub style: ShapeCell<StylePayloadsRef>,
}

impl Default for NodeData {
    fn default() -> Self {
        Self {
            parent: ShapeCell::new(NodeSlotId::INVALID),
            first_child: ShapeCell::new(NodeSlotId::INVALID),
            last_child: ShapeCell::new(NodeSlotId::INVALID),
            previous_sibling: ShapeCell::new(NodeSlotId::INVALID),
            next_sibling: ShapeCell::new(NodeSlotId::INVALID),
            kind: ShapeCell::new(NodeKind::Unset),
            generated_for: ShapeCell::new(0),
            intrinsic_cache_epoch: Cell::new(0),
            flags: ShapeCell::new(0),
            slot_generation: ShapeCell::new(0),
            compositor_animation_frame_kinds: ShapeCell::new(0),
            table_column_span: Cell::new(1),
            table_row_span: Cell::new(1),
            dom_paint_facts: ShapeCell::new(0),
            ancestor_facts: Cell::new(0),
            fragment_cache_epoch: Cell::new(0),
            style: ShapeCell::new(StylePayloadsRef::null()),
        }
    }
}

/// The part of a node the paint side reads, as a row of the arena's published column.
#[derive(Clone, PartialEq)]
pub(crate) struct PaintNode {
    pub(crate) generation: u8,
    pub(crate) kind: NodeKind,
    pub(crate) generated_for: u8,
    pub(crate) dom_paint_facts: u8,
    pub(crate) compositor_animation_frame_kinds: u8,
    pub(crate) flags: u32,
    pub(crate) parent: NodeSlotId,
    pub(crate) first_child: NodeSlotId,
    pub(crate) last_child: NodeSlotId,
    pub(crate) previous_sibling: NodeSlotId,
    pub(crate) next_sibling: NodeSlotId,
    pub(crate) style: StylePayloadsRef,
    /// The style record the row is built from, or 0.
    pub(crate) style_record: u64,
    /// The node whose style the row carries.
    pub(crate) style_node: Option<crate::css::style::tree::StyleNodeID>,
    /// Whether the arena derived the row's style record: an anonymous box's, or a style layout adjusted.
    pub(crate) style_is_derived: bool,
}

impl Default for PaintNode {
    fn default() -> Self {
        Self {
            generation: 0,
            kind: NodeKind::Unset,
            generated_for: 0,
            dom_paint_facts: 0,
            compositor_animation_frame_kinds: 0,
            flags: 0,
            parent: NodeSlotId::INVALID,
            first_child: NodeSlotId::INVALID,
            last_child: NodeSlotId::INVALID,
            previous_sibling: NodeSlotId::INVALID,
            next_sibling: NodeSlotId::INVALID,
            style: StylePayloadsRef::null(),
            style_record: 0,
            style_node: None,
            style_is_derived: false,
        }
    }
}

impl PaintNode {
    /// The node's row as `data` holds it, with the style record and node the arena keeps beside it, and whether it
    /// derived the record.
    pub(crate) fn of(
        data: &NodeData,
        style_record: u64,
        style_node: Option<crate::css::style::tree::StyleNodeID>,
        style_is_derived: bool,
    ) -> Self {
        Self {
            generation: data.slot_generation.get(),
            kind: data.kind.get(),
            generated_for: data.generated_for.get(),
            dom_paint_facts: data.dom_paint_facts.get(),
            compositor_animation_frame_kinds: data.compositor_animation_frame_kinds.get(),
            flags: data.flags.get(),
            parent: data.parent.get(),
            first_child: data.first_child.get(),
            last_child: data.last_child.get(),
            previous_sibling: data.previous_sibling.get(),
            next_sibling: data.next_sibling.get(),
            style: data.style.get(),
            style_record,
            style_node,
            style_is_derived,
        }
    }

    /// The node's computed style, or `None` for a node without one.
    pub(crate) fn style(&self) -> Option<crate::css::computed_value_views::ComputedValuesView<'_>> {
        // SAFETY: No style record is released while a recording reads the rows published for it.
        let payloads = unsafe { style_payloads(self.style) }?;
        Some(crate::css::computed_value_views::ComputedValuesView::new(
            &payloads.groups,
        ))
    }
}

#[cfg(test)]
mod tests {
    use crate::layout::node_data::{MAX_NODE_SLOT_COUNT, NodeData, NodeFlag, NodeKind, NodeSlotId};

    #[test]
    fn node_kind_has_a_stable_default_and_byte_width() {
        assert_eq!(std::mem::size_of::<NodeKind>(), 1);
        assert_eq!(NodeData::default().kind.get(), NodeKind::Unset);
    }

    #[test]
    fn intrinsic_cache_epoch_uses_existing_node_data_padding() {
        assert_eq!(std::mem::size_of::<NodeData>(), 48);
        assert_eq!(std::mem::offset_of!(NodeData, intrinsic_cache_epoch), 22);
        assert_eq!(std::mem::offset_of!(NodeData, flags), 24);
        assert_eq!(std::mem::offset_of!(NodeData, fragment_cache_epoch), 28);
        assert_eq!(std::mem::offset_of!(NodeData, slot_generation), 32);
        assert_eq!(std::mem::offset_of!(NodeData, compositor_animation_frame_kinds), 33);
        assert_eq!(std::mem::offset_of!(NodeData, table_column_span), 34);
        assert_eq!(std::mem::offset_of!(NodeData, table_row_span), 36);
        assert_eq!(std::mem::offset_of!(NodeData, dom_paint_facts), 38);
        assert_eq!(std::mem::offset_of!(NodeData, ancestor_facts), 39);
        assert_eq!(std::mem::offset_of!(NodeData, style), 40);
    }

    #[test]
    fn node_slot_id_packs_a_24_bit_index_and_an_8_bit_generation() {
        let id = NodeSlotId::new(MAX_NODE_SLOT_COUNT - 1, u8::MAX);
        assert_eq!(id.slot_index(), MAX_NODE_SLOT_COUNT - 1);
        assert_eq!(id.generation(), u8::MAX);
        assert_ne!(id, NodeSlotId::INVALID);
    }

    #[test]
    fn list_marker_position_uses_expected_flag_bit() {
        assert_eq!(NodeFlag::ListMarkerIsInside as u32, 1 << 23);
    }

    #[test]
    fn committed_fragment_link_flag_uses_a_previously_unassigned_bit() {
        assert_eq!(NodeFlag::HasCommittedFragmentLink as u32, 1 << 26);
        assert_eq!(NodeFlag::HasPreserve3dTransformStyle as u32, 1 << 27);
    }

    #[test]
    fn stamped_fact_flags_use_previously_unassigned_bits() {
        assert_eq!(NodeFlag::IsHtmlInputElement as u32, 1 << 13);
        assert_eq!(NodeFlag::IsHtmlHtmlElement as u32, 1 << 14);
        assert_eq!(NodeFlag::IsInUserAgentShadowTree as u32, 1 << 15);
        assert_eq!(NodeFlag::UsesButtonLayout as u32, 1 << 16);
        assert_eq!(NodeFlag::IsEditingHost as u32, 1 << 17);
        assert_eq!(NodeFlag::ReplacedBoxCanHaveChildren as u32, 1 << 18);
        assert_eq!(NodeFlag::ProducesLineBoxFragmentWhenEmpty as u32, 1 << 22);
        assert_eq!(NodeFlag::IsDocumentElement as u32, 1 << 30);
    }
}
