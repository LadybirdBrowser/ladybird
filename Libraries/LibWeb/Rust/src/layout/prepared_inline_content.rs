/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::fc_run_cache::{FcRunCacheValidity, run_root_validity};
use super::inline_level_iterator::ItemType;
use super::*;
use libgfx_rust::font::FontHandle;
use libgfx_rust::text_layout::SharedShape;
use std::sync::Arc;

pub(crate) struct PreparedInlineContent {
    pub(super) validity: FcRunCacheValidity,
    pub(super) items: Box<[PreparedItem]>,
    pub(super) box_events: Box<[InlineBoxEvent]>,
    pub(super) fonts: Box<[FontHandle]>,
    pub(super) tab_stops: Box<[PreparedTabStop]>,
}

const _: () = {
    const fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<PreparedInlineContent>();
};

#[derive(Clone, Debug, PartialEq)]
pub(super) struct PreparedItem {
    shape: Option<SharedShape>,
    node: Node,
    offset_in_node: u32,
    length_in_node: u32,
    bits: u16,
    /// For a text item, which of [`PreparedInlineContent::fonts`] its text is set in.
    font_index: u16,
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

    pub(super) fn new(type_: ItemType, node: Node, flags: u16) -> Self {
        Self {
            shape: None,
            node,
            offset_in_node: 0,
            length_in_node: 0,
            bits: type_ as u16 | flags,
            font_index: 0,
        }
    }

    pub(super) fn text(
        node: Node,
        text_range: std::ops::Range<usize>,
        shape: SharedShape,
        text_type: u8,
        font_index: u16,
        flags: u16,
    ) -> Self {
        debug_assert!(u16::from(text_type) <= Self::TEXT_TYPE_MASK);
        Self {
            shape: Some(shape),
            node,
            offset_in_node: u32::try_from(text_range.start).expect("text offsets fit in 32 bits"),
            length_in_node: u32::try_from(text_range.len()).expect("text lengths fit in 32 bits"),
            bits: ItemType::Text as u16 | u16::from(text_type) << Self::TEXT_TYPE_SHIFT | flags,
            font_index,
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

#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) struct InlineBoxEvent {
    position: u32,
    entered_box: Node,
}

const _: () = assert!(size_of::<InlineBoxEvent>() == 8);

impl InlineBoxEvent {
    const LEAVES_BEFORE_ITEM: u32 = 1 << 31;

    pub(super) fn position_at(item_index: usize, within_item: bool) -> u32 {
        let position = u32::try_from(item_index << 1 | usize::from(within_item)).expect("item indices fit in 31 bits");
        assert!(position < Self::LEAVES_BEFORE_ITEM, "item indices fit in 30 bits");
        position
    }

    pub(super) fn entering(position: u32, node: Node) -> Self {
        Self {
            position,
            entered_box: node,
        }
    }

    pub(super) fn leaving_innermost_box(position: u32, before_item: bool) -> Self {
        Self {
            position: position | if before_item { Self::LEAVES_BEFORE_ITEM } else { 0 },
            entered_box: NodeSlotId::INVALID,
        }
    }

    pub(super) fn position(&self) -> u32 {
        self.position & !Self::LEAVES_BEFORE_ITEM
    }

    /// Whether the box ended before the item at this position began, instead of with that item.
    pub(super) fn leaves_before_item(&self) -> bool {
        self.position & Self::LEAVES_BEFORE_ITEM != 0
    }

    pub(super) fn entered_box(&self) -> Option<Node> {
        (!self.entered_box.is_invalid()).then_some(self.entered_box)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
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
    /// Where `font` is among the fonts of the content, adding it the first time.
    pub(super) fn index_of_font(&mut self, font: &FontHandle) -> u16 {
        let index = match self.fonts.iter().rposition(|kept| kept == font) {
            Some(index) => index,
            None => {
                self.fonts.push(font.clone());
                self.fonts.len() - 1
            }
        };
        u16::try_from(index).expect("inline content is set in fewer than 65536 fonts")
    }

    pub(super) fn take_prepared_inline_content(&mut self, validity: FcRunCacheValidity) -> PreparedInlineContent {
        self.box_stack.clear();
        PreparedInlineContent {
            validity,
            items: self.items.drain(..).collect(),
            box_events: self.box_events.drain(..).collect(),
            fonts: self.fonts.drain(..).collect(),
            tab_stops: self.tab_stops.drain(..).collect(),
        }
    }
}

impl PreparedInlineContent {
    /// The font the text of `item`, one of this content's text items, is set in.
    pub(super) fn font_of(&self, item: &PreparedItem) -> &FontHandle {
        &self.fonts[usize::from(item.font_index)]
    }

    pub(crate) fn for_block_container(pass: LayoutPass<'_>, block_container: Node) -> Arc<Self> {
        if pass.reads_no_memoized_layout() || pass.lays_out_with_position_option() {
            return Arc::new(Self::prepare(pass, block_container));
        }
        let memos = pass.prepared_inline_content_memos();
        if let Some(kept) = memos.kept(block_container, run_root_validity(&pass, block_container)) {
            if cfg!(debug_assertions) || run_repeatability::verification_is_enabled() {
                kept.verify_against_fresh_preparation(pass, block_container);
            }
            return kept;
        }
        let prepared = Arc::new(Self::prepare(pass, block_container));
        if !prepared.is_empty() {
            memos.keep(block_container, prepared.clone());
        }
        prepared
    }

    fn is_empty(&self) -> bool {
        self.items.is_empty() && self.box_events.is_empty()
    }

    fn verify_against_fresh_preparation(&self, pass: LayoutPass<'_>, block_container: Node) {
        let fresh = Self::prepare(pass, block_container);
        if let Some(difference) = self.first_difference_from(&fresh) {
            panic!(
                "the inline content kept for {block_container:?} differs from its fresh preparation in {difference}"
            );
        }
    }

    fn first_difference_from(&self, fresh: &Self) -> Option<String> {
        fn first_difference<T: PartialEq + std::fmt::Debug>(name: &str, kept: &[T], fresh: &[T]) -> Option<String> {
            let index = (0..kept.len().max(fresh.len())).find(|index| kept.get(*index) != fresh.get(*index))?;
            Some(format!(
                "{name} {index}: kept {:?}, prepared {:?}",
                kept.get(index),
                fresh.get(index)
            ))
        }
        first_difference("item", &self.items, &fresh.items)
            .or_else(|| first_difference("inline box event", &self.box_events, &fresh.box_events))
            .or_else(|| first_difference("font", &self.fonts, &fresh.fonts))
            .or_else(|| first_difference("tab stop", &self.tab_stops, &fresh.tab_stops))
    }
}

#[derive(Default)]
pub(crate) struct PreparedInlineContentMemos {
    by_slot: super::pass_column::ForkSharedRows<Option<Arc<PreparedInlineContent>>>,
}

impl PreparedInlineContentMemos {
    pub(crate) fn fork(&self) -> Self {
        Self {
            by_slot: self.by_slot.fork(),
        }
    }

    pub(crate) fn kept(
        &self,
        block_container: Node,
        validity: FcRunCacheValidity,
    ) -> Option<Arc<PreparedInlineContent>> {
        self.by_slot.with_row(block_container.slot_index() as usize, |kept| {
            kept?.as_ref().filter(|kept| kept.validity == validity).cloned()
        })
    }

    pub(crate) fn keep(&self, block_container: Node, prepared: Arc<PreparedInlineContent>) {
        self.by_slot
            .with_row_mut(block_container.slot_index() as usize, |kept| *kept = Some(prepared));
    }

    pub(super) fn cover_slots(&mut self, slot_count: usize) {
        self.by_slot.cover_slots(slot_count);
    }

    pub(crate) fn forget(&mut self, slot: u32) {
        self.by_slot.reset_row_where(slot as usize, Option::is_some);
    }
}
