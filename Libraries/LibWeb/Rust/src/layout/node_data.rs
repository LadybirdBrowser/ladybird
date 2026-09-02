/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::CssPixels;
use std::cell::Cell;
use std::ffi::c_void;

pub const INVALID_NODE_SLOT_INDEX: u32 = u32::MAX;
pub const GENERATED_FOR_AFTER: u8 = 1;
pub const GENERATED_FOR_MARKER: u8 = 6;

// The full C++ StyleGroupIndex space; LayoutRustBridge.cpp static-asserts the
// count so the style container array and the registered group indices line up.
pub const STYLE_GROUP_COUNT: usize = 23;

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

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStylePayloads {
    pub groups: [*const c_void; STYLE_GROUP_COUNT],
}

impl Default for FfiStylePayloads {
    fn default() -> Self {
        Self {
            groups: [std::ptr::null(); STYLE_GROUP_COUNT],
        }
    }
}

const NODE_SLOT_INDEX_BITS: u32 = 24;
const NODE_SLOT_INDEX_MASK: u32 = (1 << NODE_SLOT_INDEX_BITS) - 1;
pub(crate) const MAX_NODE_SLOT_COUNT: u32 = NODE_SLOT_INDEX_MASK;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
#[repr(C)]
pub struct NodeSlotId {
    pub index: u32,
}

impl NodeSlotId {
    pub const INVALID: Self = Self {
        index: INVALID_NODE_SLOT_INDEX,
    };

    pub(crate) fn new(index: u32, generation: u8) -> Self {
        assert!(
            index < MAX_NODE_SLOT_COUNT,
            "layout node arena exhausted its 24-bit slot index space"
        );
        assert_ne!(generation, 0, "layout node arena slot generation must be nonzero");
        Self {
            index: index | (u32::from(generation) << NODE_SLOT_INDEX_BITS),
        }
    }

    pub(crate) fn slot_index(self) -> u32 {
        self.index & NODE_SLOT_INDEX_MASK
    }

    pub(crate) fn generation(self) -> u8 {
        (self.index >> NODE_SLOT_INDEX_BITS) as u8
    }

    pub fn is_invalid(self) -> bool {
        self == Self::INVALID
    }
}

impl Default for NodeSlotId {
    fn default() -> Self {
        Self::INVALID
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
    NodeWithStyle = 16,
    RadioButton = 18,
    RangeInputBox = 19,
    ReplacedBox = 20,
    SVGBox = 21,
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
    TextSliceNode = 36,
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
    HasSavedAbsposLayoutInputs = 1 << 19,
    SavedAbsposCbDerivesFromOwnComputedValues = 1 << 20,
    SavedAbsposAlignmentDerivesFromOwnComputedValues = 1 << 21,
    ProducesLineBoxFragmentWhenEmpty = 1 << 22,
    ListMarkerIsInside = 1 << 23,
    HasAnchorNames = 1 << 24,
    InsetsUseAnchorFunctions = 1 << 25,
    HasCommittedFragmentLink = 1 << 26,
    HasPreserve3dTransformStyle = 1 << 27,
    IsMissingTableCell = 1 << 28,
    HasAnimatedOpacityOrTransform = 1 << 29,
    IsDocumentElement = 1 << 30,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum FfiNodeLink {
    Parent,
    FirstChild,
    LastChild,
    PreviousSibling,
    NextSibling,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiNodeConstructionFacts {
    pub kind: NodeKind,
    pub shell: *mut c_void,
    pub is_anonymous: bool,
    pub is_html_input_element: bool,
    pub is_html_html_element: bool,
    pub is_document_element: bool,
    pub is_in_user_agent_shadow_tree: bool,
    pub uses_button_layout: bool,
    pub is_editing_host: bool,
    pub is_body: bool,
}

#[repr(C)]
pub(crate) struct NodeData {
    pub parent: Cell<NodeSlotId>,
    pub first_child: Cell<NodeSlotId>,
    pub last_child: Cell<NodeSlotId>,
    pub previous_sibling: Cell<NodeSlotId>,
    pub next_sibling: Cell<NodeSlotId>,
    pub containing_block: Cell<NodeSlotId>,
    pub inline_containing_block: Cell<NodeSlotId>,
    pub kind: Cell<NodeKind>,
    pub generated_for: Cell<u8>,
    pub intrinsic_cache_epoch: Cell<u16>,
    pub flags: Cell<u32>,
    /// Advanced on every layout invalidation that reaches this node or its
    /// subtree, with no propagation boundary: unlike the intrinsic epoch,
    /// changes inside absolutely positioned and SVG descendants must reach
    /// every ancestor, because their fragments live in ancestor run trees.
    /// Wide enough that wrapping between a cache store and the next probe
    /// is unreachable.
    pub fragment_cache_epoch: Cell<u32>,
    pub slot_generation: Cell<u8>,
    pub table_column_span: Cell<u16>,
    pub table_row_span: Cell<u16>,
    pub style: Cell<*const c_void>,
    pub shell: Cell<*mut c_void>,
}

impl Default for NodeData {
    fn default() -> Self {
        Self {
            parent: Cell::new(NodeSlotId::INVALID),
            first_child: Cell::new(NodeSlotId::INVALID),
            last_child: Cell::new(NodeSlotId::INVALID),
            previous_sibling: Cell::new(NodeSlotId::INVALID),
            next_sibling: Cell::new(NodeSlotId::INVALID),
            containing_block: Cell::new(NodeSlotId::INVALID),
            inline_containing_block: Cell::new(NodeSlotId::INVALID),
            kind: Cell::new(NodeKind::Unset),
            generated_for: Cell::new(0),
            intrinsic_cache_epoch: Cell::new(0),
            flags: Cell::new(0),
            slot_generation: Cell::new(0),
            table_column_span: Cell::new(1),
            table_row_span: Cell::new(1),
            fragment_cache_epoch: Cell::new(0),
            style: Cell::new(std::ptr::null()),
            shell: Cell::new(std::ptr::null_mut()),
        }
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
        assert_eq!(std::mem::size_of::<NodeData>(), 64);
        assert_eq!(std::mem::offset_of!(NodeData, intrinsic_cache_epoch), 30);
        assert_eq!(std::mem::offset_of!(NodeData, flags), 32);
        assert_eq!(std::mem::offset_of!(NodeData, fragment_cache_epoch), 36);
        assert_eq!(std::mem::offset_of!(NodeData, slot_generation), 40);
        assert_eq!(std::mem::offset_of!(NodeData, table_column_span), 42);
        assert_eq!(std::mem::offset_of!(NodeData, table_row_span), 44);
        assert_eq!(std::mem::offset_of!(NodeData, style), 48);
        assert_eq!(std::mem::offset_of!(NodeData, shell), 56);
    }

    #[test]
    fn node_slot_id_packs_a_24_bit_index_and_an_8_bit_generation() {
        let id = NodeSlotId::new(MAX_NODE_SLOT_COUNT - 1, u8::MAX);
        assert_eq!(id.slot_index(), MAX_NODE_SLOT_COUNT - 1);
        assert_eq!(id.generation(), u8::MAX);
        assert_ne!(id, NodeSlotId::INVALID);
    }

    #[test]
    fn saved_abspos_flags_use_previously_unassigned_bits() {
        assert_eq!(NodeFlag::IsReplacedElement as u32, 1 << 12);
        assert_eq!(NodeFlag::HasSavedAbsposLayoutInputs as u32, 1 << 19);
        assert_eq!(NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues as u32, 1 << 20);
        assert_eq!(
            NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues as u32,
            1 << 21
        );
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
