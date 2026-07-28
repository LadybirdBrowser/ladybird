/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;

pub const INVALID_NODE_SLOT_INDEX: u32 = u32::MAX;
pub const GENERATED_FOR_MARKER: u8 = 6;

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
    NodeWithStyleAndBoxModelMetrics = 17,
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
    HasBeenWrappedInTableWrapper = 1 << 5,
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
    OwnStyleEstablishesBlockFormattingContext = 1 << 19,
    HasSavedAbsposLayoutInputs = 1 << 20,
    SavedAbsposCbDerivesFromOwnComputedValues = 1 << 21,
    SavedAbsposAlignmentDerivesFromOwnComputedValues = 1 << 22,
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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum NodeDisplayFlag {
    InlineOutside = 1 << 0,
    FlowInside = 1 << 1,
    FlexInside = 1 << 2,
    GridInside = 1 << 3,
    Floating = 1 << 4,
    AbsolutelyPositioned = 1 << 5,
    MathInside = 1 << 6,
    BlockOutsideBeforeBoxTypeTransformation = 1 << 7,
}

#[repr(C)]
pub struct NodeData {
    pub parent: NodeSlotId,
    pub first_child: NodeSlotId,
    pub last_child: NodeSlotId,
    pub previous_sibling: NodeSlotId,
    pub next_sibling: NodeSlotId,
    pub containing_block: NodeSlotId,
    pub inline_containing_block: NodeSlotId,
    pub kind: NodeKind,
    pub generated_for: u8,
    pub intrinsic_cache_epoch: u16,
    pub flags: u32,
    pub initial_quote_nesting_level: u32,
    pub table_display: FfiTableDisplay,
    pub table_display_before: FfiTableDisplay,
    pub display_bits: u8,
    pub slot_generation: u8,
    pub style: *const c_void,
    pub shell: *mut c_void,
}

impl Default for NodeData {
    fn default() -> Self {
        Self {
            parent: NodeSlotId::INVALID,
            first_child: NodeSlotId::INVALID,
            last_child: NodeSlotId::INVALID,
            previous_sibling: NodeSlotId::INVALID,
            next_sibling: NodeSlotId::INVALID,
            containing_block: NodeSlotId::INVALID,
            inline_containing_block: NodeSlotId::INVALID,
            kind: NodeKind::Unset,
            generated_for: 0,
            intrinsic_cache_epoch: 0,
            flags: 0,
            initial_quote_nesting_level: 0,
            table_display: FfiTableDisplay::Other,
            table_display_before: FfiTableDisplay::Other,
            display_bits: 0,
            slot_generation: 0,
            style: std::ptr::null(),
            shell: std::ptr::null_mut(),
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::layout::node_data::{MAX_NODE_SLOT_COUNT, NodeData, NodeDisplayFlag, NodeFlag, NodeKind, NodeSlotId};

    #[test]
    fn node_kind_has_a_stable_default_and_byte_width() {
        assert_eq!(std::mem::size_of::<NodeKind>(), 1);
        assert_eq!(NodeData::default().kind, NodeKind::Unset);
    }

    #[test]
    fn intrinsic_cache_epoch_uses_existing_node_data_padding() {
        assert_eq!(std::mem::size_of::<NodeData>(), 64);
        assert_eq!(std::mem::offset_of!(NodeData, intrinsic_cache_epoch), 30);
        assert_eq!(std::mem::offset_of!(NodeData, flags), 32);
        assert_eq!(std::mem::offset_of!(NodeData, slot_generation), 43);
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
        assert_eq!(NodeFlag::HasSavedAbsposLayoutInputs as u32, 1 << 20);
        assert_eq!(NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues as u32, 1 << 21);
        assert_eq!(
            NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues as u32,
            1 << 22
        );
    }

    #[test]
    fn stamped_fact_flags_use_previously_unassigned_bits() {
        assert_eq!(NodeFlag::IsHtmlInputElement as u32, 1 << 13);
        assert_eq!(NodeFlag::IsHtmlHtmlElement as u32, 1 << 14);
        assert_eq!(NodeFlag::IsInUserAgentShadowTree as u32, 1 << 15);
        assert_eq!(NodeFlag::UsesButtonLayout as u32, 1 << 16);
        assert_eq!(NodeFlag::IsEditingHost as u32, 1 << 17);
        assert_eq!(NodeFlag::ReplacedBoxCanHaveChildren as u32, 1 << 18);
        assert_eq!(NodeFlag::OwnStyleEstablishesBlockFormattingContext as u32, 1 << 19);
        assert_eq!(NodeDisplayFlag::BlockOutsideBeforeBoxTypeTransformation as u8, 1 << 7);
    }
}
