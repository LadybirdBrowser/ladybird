/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::inline_level_iterator::ItemType;
use super::*;
use libgfx_rust::font::FontHandle;
use libgfx_rust::text_layout::SharedShape;

pub(crate) struct PreparedInlineContent {
    pub(super) items: Box<[PreparedItem]>,
    pub(super) box_events: Box<[InlineBoxEvent]>,
    pub(super) fonts: Box<[FontHandle]>,
    pub(super) tab_stops: Box<[PreparedTabStop]>,
}

const _: () = {
    const fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<PreparedInlineContent>();
};

#[derive(Clone)]
pub(super) struct PreparedItem {
    shape: Option<SharedShape>,
    node: Node,
    offset_in_node: u32,
    length_in_node: u32,
    bits: u16,
}

const _: () = assert!(size_of::<PreparedItem>() == 24);

const ITEM_TYPES_BY_DISCRIMINANT: [ItemType; 6] = [
    ItemType::Text,
    ItemType::Element,
    ItemType::BlockLevelBox,
    ItemType::ForcedBreak,
    ItemType::AbsolutelyPositionedElement,
    ItemType::FloatingElement,
];

const _: () = {
    let mut discriminant = 0;
    while discriminant < ITEM_TYPES_BY_DISCRIMINANT.len() {
        assert!(ITEM_TYPES_BY_DISCRIMINANT[discriminant] as usize == discriminant);
        discriminant += 1;
    }
};

impl PreparedItem {
    const ITEM_TYPE_MASK: u16 = 0b111;
    const TEXT_TYPE_SHIFT: u16 = 3;
    const TEXT_TYPE_MASK: u16 = 0b111;
    pub(super) const COLLAPSIBLE_WHITESPACE: u16 = 1 << 6;
    pub(super) const CAN_BREAK_BEFORE: u16 = 1 << 7;
    pub(super) const TRAILING_WHITESPACE_IS_WHOLE_ITEM: u16 = 1 << 8;
    pub(super) const TAKES_LEADING_EDGES: u16 = 1 << 9;
    pub(super) const TAKES_TRAILING_EDGES: u16 = 1 << 10;
    pub(super) const UPRIGHT: u16 = 1 << 11;
    pub(super) const TAB_STOP: u16 = 1 << 12;
    pub(super) const TAKES_NEXT_FONT: u16 = 1 << 13;

    pub(super) fn new(type_: ItemType, node: Node, flags: u16) -> Self {
        Self {
            shape: None,
            node,
            offset_in_node: 0,
            length_in_node: 0,
            bits: type_ as u16 | flags,
        }
    }

    pub(super) fn text(
        node: Node,
        text_range: std::ops::Range<usize>,
        shape: SharedShape,
        text_type: u8,
        flags: u16,
    ) -> Self {
        debug_assert!(u16::from(text_type) <= Self::TEXT_TYPE_MASK);
        Self {
            shape: Some(shape),
            node,
            offset_in_node: u32::try_from(text_range.start).expect("text offsets fit in 32 bits"),
            length_in_node: u32::try_from(text_range.len()).expect("text lengths fit in 32 bits"),
            bits: ItemType::Text as u16 | u16::from(text_type) << Self::TEXT_TYPE_SHIFT | flags,
        }
    }

    pub(super) fn type_(&self) -> ItemType {
        ITEM_TYPES_BY_DISCRIMINANT[usize::from(self.bits & Self::ITEM_TYPE_MASK)]
    }

    pub(super) fn node(&self) -> Node {
        self.node
    }

    pub(super) fn offset_in_node(&self) -> usize {
        self.offset_in_node as usize
    }

    pub(super) fn length_in_node(&self) -> usize {
        self.length_in_node as usize
    }

    #[track_caller]
    pub(super) fn shape(&self) -> &SharedShape {
        self.shape.as_ref().expect("a text item carries its shaped text")
    }

    pub(super) fn text_type(&self) -> u8 {
        ((self.bits >> Self::TEXT_TYPE_SHIFT) & Self::TEXT_TYPE_MASK) as u8
    }

    pub(super) fn has(&self, flag: u16) -> bool {
        self.bits & flag != 0
    }
}

#[derive(Clone, Copy)]
pub(super) struct InlineBoxEvent {
    position: u32,
    entered_box: Node,
}

const _: () = assert!(size_of::<InlineBoxEvent>() == 8);

impl InlineBoxEvent {
    pub(super) fn position_at(item_index: usize, within_item: bool) -> u32 {
        u32::try_from(item_index << 1 | usize::from(within_item)).expect("item indices fit in 31 bits")
    }

    pub(super) fn entering(position: u32, node: Node) -> Self {
        Self {
            position,
            entered_box: node,
        }
    }

    pub(super) fn leaving_innermost_box(position: u32) -> Self {
        Self {
            position,
            entered_box: NodeSlotId::INVALID,
        }
    }

    pub(super) fn position(&self) -> u32 {
        self.position
    }

    pub(super) fn entered_box(&self) -> Option<Node> {
        (!self.entered_box.is_invalid()).then_some(self.entered_box)
    }
}

#[derive(Clone, Copy)]
pub(super) struct PreparedTabStop {
    pub(super) tab_count: u32,
    pub(super) tab_inline_size: CssPixels,
    pub(super) zero_advance: f32,
    pub(super) upright_advance: Option<f32>,
}

const _: () = assert!(size_of::<PreparedTabStop>() == 20);

#[derive(Default)]
pub(crate) struct InlineContentPreparationBuffers {
    pub(super) items: Vec<PreparedItem>,
    pub(super) box_events: Vec<InlineBoxEvent>,
    pub(super) fonts: Vec<FontHandle>,
    pub(super) tab_stops: Vec<PreparedTabStop>,
    pub(super) box_stack: Vec<Node>,
}

impl InlineContentPreparationBuffers {
    pub(super) fn take_prepared_inline_content(&mut self) -> PreparedInlineContent {
        self.box_stack.clear();
        PreparedInlineContent {
            items: self.items.drain(..).collect(),
            box_events: self.box_events.drain(..).collect(),
            fonts: self.fonts.drain(..).collect(),
            tab_stops: self.tab_stops.drain(..).collect(),
        }
    }
}
