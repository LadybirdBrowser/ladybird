/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Layout styles share the style engine's immutable records. Derivation edits only the
//! affected groups, and publishes final animated payloads as an independent base record.

use super::StyleEngine;
use crate::css::computed_value_types::*;
use crate::css::css_enums::{
    display_inside, display_internal, display_outside, flex_direction, justify_content, vertical_align,
};
use crate::css::display::FfiDisplay;
use crate::css::table_group_builder::group_index;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AnonymousStyleKind {
    Wrapper,
    TableRow,
    TableCell,
    Table,
    InlineTable,
    MissingTableCell,
    TableWrapper,
    ButtonFlexWrapper,
    ButtonContentBox,
    FieldsetContentWrapper,
    InlineStyleWrapper,
    /// The inline box a writing suggestion's text renders in after the caret.
    WritingSuggestion,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct AnonymousStyleOverrides {
    pub overflow_x: u8,
    pub overflow_y: u8,
    pub columns_from_parent: bool,
}

/// What an anonymous box derives its record from as it is styled again: the record it held, its parent's, and the one
/// it takes its column properties from, if any.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct Reinheritance {
    record: u64,
    parent: u64,
    column_source: Option<u64>,
}

#[derive(Clone, Copy)]
pub(crate) struct DerivedStyleRecord {
    pub record: u64,
    pub payloads: crate::layout::node_data::LayoutStyleRef,
}

impl DerivedStyleRecord {
    /// Pins `record` for a layout row and names its group payloads.
    pub(crate) fn pin(engine: &mut StyleEngine, record: u64) -> Self {
        engine.pin_layout_style_record(record);
        let payloads = engine
            .style_record_payloads(record)
            .expect("a layout style record is live");
        Self {
            record,
            payloads: crate::layout::node_data::LayoutStyleRef::of_groups(payloads),
        }
    }
}

trait LayoutStyleGroup: Clone + PartialEq {
    const INDEX: usize;
}

macro_rules! groups {
    ($($ty:ty => $index:ident),+ $(,)?) => {
        $(impl LayoutStyleGroup for $ty { const INDEX: usize = group_index::$index; })+
    };
}

groups! {
    BoxValues => BOX,
    AlignmentValues => ALIGNMENT,
    SurroundValues => SURROUND,
    SizingValues => SIZING,
    GridValues => GRID,
    MiscLayoutValues => MISC_LAYOUT,
    MiscInteractionValues => MISC_INTERACTION,
    EffectsValues => EFFECTS,
    AnchorValues => ANCHOR,
    BorderValues => BORDER,
    InheritedTextValues => INHERITED_TEXT,
}

/// A style for layout's own use, which no record holds: a base style's groups, with those an edit
/// wrote replaced by copies. It is never interned or installed.
#[cfg(test)]
pub(crate) struct LayoutScratchStyle(crate::css::computed_values::StyleGroups);

#[cfg(test)]
impl LayoutScratchStyle {
    /// `base`, with the groups `edit` writes replaced, the rest shared.
    pub(crate) fn over(
        base: crate::css::computed_value_views::ComputedValuesView<'_>,
        edit: impl FnOnce(&mut ScratchGroups),
    ) -> Self {
        let mut groups = ScratchGroups {
            groups: base.record_groups().0.clone(),
            written: [false; group_index::COUNT],
        };
        edit(&mut groups);
        Self(groups.groups)
    }

    /// The default groups, with the groups `edit` writes replaced.
    pub(crate) fn over_defaults(edit: impl FnOnce(&mut ScratchGroups)) -> Self {
        let defaults = crate::css::computed_values::DefaultGroups::default();
        Self::over(
            crate::css::computed_value_views::ComputedValuesView::of(defaults.groups()),
            edit,
        )
    }

    /// The style, for a row to read while this stays in place.
    pub(crate) fn style(&self) -> crate::layout::node_data::LayoutStyleRef {
        crate::layout::node_data::LayoutStyleRef::of_groups(&self.0)
    }
}

/// The groups of a [`LayoutScratchStyle`] being written: each group is copied the first time it is
/// written, and the copy is the scratch style's own.
#[cfg(test)]
pub(crate) struct ScratchGroups {
    groups: crate::css::computed_values::StyleGroups,
    written: [bool; group_index::COUNT],
}

#[cfg(test)]
impl ScratchGroups {
    fn group_mut<T: LayoutStyleGroup>(&mut self) -> &mut T {
        if !self.written[T::INDEX] {
            // SAFETY: The private trait pairs each type with its registered group index.
            unsafe { self.groups.write_group::<T>(T::INDEX, |_| {}) };
            self.written[T::INDEX] = true;
        }
        // SAFETY: The group is a copy only these groups hold, written through `&mut self` alone, and
        //         the private trait pairs each type with its registered group index.
        unsafe { &mut *self.groups[T::INDEX].as_ptr().cast_mut().cast::<T>() }
    }

    pub(crate) fn box_mut(&mut self) -> &mut BoxValues {
        self.group_mut()
    }

    pub(crate) fn sizing_mut(&mut self) -> &mut SizingValues {
        self.group_mut()
    }

    pub(crate) fn surround_mut(&mut self) -> &mut SurroundValues {
        self.group_mut()
    }

    pub(crate) fn alignment_mut(&mut self) -> &mut AlignmentValues {
        self.group_mut()
    }

    pub(crate) fn border_mut(&mut self) -> &mut BorderValues {
        self.group_mut()
    }

    pub(crate) fn set_white_space_collapse(&mut self, collapse: u8) {
        let mut values = self.groups.inherited_values();
        values.bits = values
            .bits
            .with(crate::css::computed_values::StyleBitField::WhiteSpaceCollapse, collapse);
        self.groups.set_inherited_values(values);
    }

    pub(crate) fn set_writing_mode(&mut self, writing_mode: u8) {
        let mut values = self.groups.inherited_values();
        values.bits = values
            .bits
            .with(crate::css::computed_values::StyleBitField::WritingMode, writing_mode);
        self.groups.set_inherited_values(values);
    }

    pub(crate) fn set_overflow(&mut self, x: u8, y: u8) {
        use crate::css::computed_values::StyleBitField;
        let reset = self
            .groups
            .reset_bits()
            .with(StyleBitField::OverflowX, x)
            .with(StyleBitField::OverflowY, y);
        self.groups.set_reset_bits(reset);
    }
}

pub(crate) struct LayoutStyle {
    payloads: crate::css::computed_values::StyleGroups,
    /// The record whose longhands the style carries, which is read only once the style is interned.
    longhand_source: Option<u64>,
    changed: bool,
}

impl LayoutStyle {
    pub(crate) fn from_record(engine: &super::RetainedState, record: u64) -> Self {
        let view = engine
            .style_record_view(record)
            .expect("layout style record must be live");
        Self {
            payloads: view.payloads.clone(),
            longhand_source: Some(record),
            changed: false,
        }
    }

    fn inherited(engine: &StyleEngine, parent: u64) -> Self {
        let parent = engine
            .style_record_payloads(parent)
            .expect("anonymous box parent style must be live");
        let mut payloads = engine.default_groups().groups().clone();
        for index in
            crate::css::computed_values::StyleGroupIndex::all().take(super::computed::ENGINE_INHERITED_GROUP_COUNT)
        {
            payloads.share_group(parent, index);
        }
        payloads.set_inherited_values(parent.inherited_values());
        Self {
            payloads,
            longhand_source: None,
            changed: false,
        }
    }

    pub(crate) fn inherit_from(&mut self, engine: &StyleEngine, parent: u64) {
        let parent = engine
            .style_record_payloads(parent)
            .expect("anonymous box parent style must be live");
        for index in
            crate::css::computed_values::StyleGroupIndex::all().take(super::computed::ENGINE_INHERITED_GROUP_COUNT)
        {
            self.payloads.share_group(parent, index);
        }
        self.payloads.set_inherited_values(parent.inherited_values());
    }

    fn group<T: LayoutStyleGroup>(&self) -> &T {
        // SAFETY: The private trait pairs each type with its registered group index.
        unsafe { &*self.payloads[T::INDEX].as_ptr().cast::<T>() }
    }

    fn edit<T: LayoutStyleGroup>(&mut self, update: impl FnOnce(&mut T)) {
        let mut value = self.group::<T>().clone();
        update(&mut value);
        if value == *self.group::<T>() {
            return;
        }
        self.changed = true;
        // SAFETY: The private trait pairs each type with its registered group index.
        unsafe { self.payloads.write_group::<T>(T::INDEX, |payload| *payload = value) };
    }

    /// Draws text in its color at half its alpha, i.e. `color-mix(in srgb, currentColor 50%, transparent)`, which
    /// reads as a suggestion in light and dark color schemes alike.
    fn fade_text_color(&mut self) {
        let fade = |color: u32| (color >> 24).div_ceil(2) << 24 | (color & 0x00ff_ffff);
        let index = group_index::INHERITED_TEXT;
        // SAFETY: The group is the inherited text group.
        unsafe {
            self.payloads.write_group::<InheritedTextValues>(index, |values| {
                values.color = fade(values.color);
                values.webkit_text_fill_color = fade(values.webkit_text_fill_color);
            });
        };
        self.changed = true;
    }

    pub(crate) fn is_unchanged(&self) -> bool {
        !self.changed
    }

    /// The group payloads of this style, for a row's style reads to see in place of its record's, as a record's
    /// payloads are named. They address the style where it is, so the caller keeps it in place while reads see them.
    pub(crate) fn payloads_for_style_reads(&self) -> crate::layout::node_data::LayoutStyleRef {
        crate::layout::node_data::LayoutStyleRef::of_groups(&self.payloads)
    }

    /// Builds the payloads of `groups` again from `table`, a longhand table holding the style's computed values with
    /// some of them replaced.
    /// The values `animated_overlay` samples over the table win over it where they do over the style's own table.
    pub(crate) fn replace_groups_from_values(
        &mut self,
        values: &crate::css::computed_values::GroupValues<'_>,
        groups: &[usize],
        defaults: &crate::css::computed_values::DefaultGroups,
    ) {
        let source = crate::css::table_group_builder::LonghandSource::Values { values, reads: None };
        for &group in groups {
            let current = self.payloads[group].as_ptr();
            // SAFETY: The style holds a reference to its payload of every group.
            let parent = unsafe { crate::css::computed_values::GroupPayloadRef::new(group, current) };
            let payload = crate::css::table_group_builder::rebuild_group_from_source(
                &source,
                group,
                crate::css::computed_values::GroupShares::new(defaults, group, parent),
                0,
                0,
                None,
            )
            .expect("a group of the accepted @position-try properties builds from its values");
            // SAFETY: The rebuild hands over a reference to a payload of the group.
            let payload = unsafe { crate::css::computed_values::HeldGroup::adopt(group, payload) };
            if payload.as_ptr() == current {
                continue;
            }
            self.changed = true;
            drop(self.payloads.replace(payload));
        }
    }

    pub(crate) fn position_area(&self) -> Vec<u8> {
        self.group::<AnchorValues>().position_area.as_slice().to_vec()
    }

    pub(crate) fn set_position_area(&mut self, codes: Vec<u8>) {
        self.edit::<AnchorValues>(|values| values.position_area = RetainedPositionAreaList::from_vec(codes));
    }

    pub(crate) fn set_display(&mut self, display: FfiDisplay) {
        self.edit::<BoxValues>(|values| values.display = display);
    }

    pub(crate) fn set_overflow(&mut self, x: u8, y: u8) {
        use crate::css::computed_values::StyleBitField;
        let reset = self
            .payloads
            .reset_bits()
            .with(StyleBitField::OverflowX, x)
            .with(StyleBitField::OverflowY, y);
        self.set_reset_bits(reset);
    }

    /// Puts `reset` in place of the style's reset bits.
    fn set_reset_bits(&mut self, reset: crate::css::computed_value_types::ResetBits) {
        if reset != self.payloads.reset_bits() {
            self.changed = true;
            self.payloads.set_reset_bits(reset);
        }
    }

    /// The style's reset bits, with the codes of `source`'s position, float and clear.
    fn with_position_float_clear_of(
        &self,
        source: crate::css::computed_value_types::ResetBits,
    ) -> crate::css::computed_value_types::ResetBits {
        use crate::css::computed_values::StyleBitField;
        [StyleBitField::Position, StyleBitField::Float, StyleBitField::Clear]
            .into_iter()
            .fold(self.payloads.reset_bits(), |bits, field| {
                bits.with(field, source.get(field))
            })
    }

    pub(crate) fn set_writing_mode_and_direction(&mut self, writing_mode: u8, direction: u8) {
        use crate::css::computed_values::StyleBitField;
        let mut values = self.payloads.inherited_values();
        values.bits = values
            .bits
            .with(StyleBitField::WritingMode, writing_mode)
            .with(StyleBitField::Direction, direction);
        if values != self.payloads.inherited_values() {
            self.changed = true;
            self.payloads.set_inherited_values(values);
        }
    }

    pub(crate) fn set_scrollbar_width(&mut self, width: u8) {
        self.edit::<MiscInteractionValues>(|values| values.scrollbar_width = width);
    }

    pub(crate) fn set_overscroll_behavior(&mut self, x: u8, y: u8) {
        self.edit::<MiscInteractionValues>(|values| {
            values.overscroll_behavior_x = x;
            values.overscroll_behavior_y = y;
        });
    }

    // CSS table wrappers carry positioning and margins, and act as the table's grid item.
    // z-index and vertical-align also follow the wrapper for interoperable painting and
    // inline alignment; clip only applies on the wrapper that receives position.
    fn transfer_table_properties(&mut self, source: &Self) {
        let box_values = source.group::<BoxValues>();
        self.edit::<BoxValues>(|target| {
            target.display = FfiDisplay::outside_and_inside(
                if box_values.display.is_inline_outside() {
                    display_outside::INLINE
                } else {
                    display_outside::BLOCK
                },
                display_inside::FLOW_ROOT,
                false,
            );
            // NB: The wrapper is the box that gets positioned, so it keeps the display the table was given, which
            //     decides where an absolutely positioned table's static position is.
            target.display_before_box_type_transformation = box_values.display_before_box_type_transformation;
            target.has_z_index = box_values.has_z_index;
            target.z_index = box_values.z_index;
            target.vertical_align = box_values.vertical_align.clone();
            target.baseline_source = box_values.baseline_source;
        });
        self.set_reset_bits(self.with_position_float_clear_of(source.payloads.reset_bits()));
        let misc_layout = source.group::<MiscLayoutValues>();
        self.edit::<MiscLayoutValues>(|target| {
            target.column_span = misc_layout.column_span;
            target.break_before = misc_layout.break_before;
            target.break_after = misc_layout.break_after;
            target.break_inside = misc_layout.break_inside;
        });
        self.copy_table_properties(source);
    }

    pub(crate) fn copy_column_properties(&mut self, source: &Self) {
        let box_values = source.group::<BoxValues>();
        self.edit::<BoxValues>(|target| {
            target.column_count = box_values.column_count;
            target.column_count_has_value = box_values.column_count_has_value;
            target.column_width = box_values.column_width.clone();
        });
        let misc_layout = source.group::<MiscLayoutValues>();
        self.edit::<MiscLayoutValues>(|target| {
            target.column_fill = misc_layout.column_fill;
            target.column_rule_style = misc_layout.column_rule_style;
            target.column_rule_width = misc_layout.column_rule_width;
            target.column_rule_color = misc_layout.column_rule_color;
        });
        let alignment = source.group::<AlignmentValues>();
        self.edit::<AlignmentValues>(|target| target.column_gap = alignment.column_gap.clone());
    }

    fn copy_table_properties(&mut self, source: &Self) {
        let surround = source.group::<SurroundValues>();
        self.edit::<SurroundValues>(|target| {
            target.inset = surround.inset.clone();
            target.position_anchor = surround.position_anchor.clone();
            target.margin = surround.margin.clone();
        });
        self.edit::<AnchorValues>(|target| {
            let source = source.group::<AnchorValues>();
            target.position_anchor_type = source.position_anchor_type;
            target.position_anchor_name = source.position_anchor_name.clone();
        });
        let alignment = source.group::<AlignmentValues>();
        self.edit::<AlignmentValues>(|target| {
            target.align_self = alignment.align_self;
            target.justify_self = alignment.justify_self;
            target.align_self_overflow = alignment.align_self_overflow;
            target.justify_self_overflow = alignment.justify_self_overflow;
            target.order = alignment.order;
        });
        self.edit::<GridValues>(|target| {
            crate::css::computed_values::copy_grid_placements(source.group::<GridValues>(), target);
        });
        self.edit::<EffectsValues>(|target| {
            let source = source.group::<EffectsValues>();
            target.clip_is_rect = source.clip_is_rect;
            if source.clip_is_rect {
                target.clip_edges = source.clip_edges;
            }
        });
    }

    pub(crate) fn reset_table_properties(&mut self, defaults: &crate::css::computed_values::DefaultGroups) {
        let defaults = Self {
            payloads: defaults.groups().clone(),
            longhand_source: None,
            changed: false,
        };
        self.set_reset_bits(self.with_position_float_clear_of(crate::css::computed_value_types::ResetBits::INITIAL));
        let initial = defaults.group::<BoxValues>();
        self.edit::<BoxValues>(|target| {
            target.has_z_index = initial.has_z_index;
            target.z_index = initial.z_index;
            target.vertical_align = initial.vertical_align.clone();
            target.baseline_source = initial.baseline_source;
        });
        self.copy_table_properties(&defaults);
    }

    pub(crate) fn anonymous(
        engine: &StyleEngine,
        parent: u64,
        kind: AnonymousStyleKind,
        overrides: AnonymousStyleOverrides,
    ) -> Self {
        let mut style = if kind == AnonymousStyleKind::InlineStyleWrapper {
            Self::from_record(engine, parent)
        } else {
            Self::inherited(engine, parent)
        };
        let block_flow = FfiDisplay::outside_and_inside(display_outside::BLOCK, display_inside::FLOW, false);
        match kind {
            AnonymousStyleKind::Wrapper | AnonymousStyleKind::ButtonContentBox => style.set_display(block_flow),
            AnonymousStyleKind::TableRow => style.set_display(FfiDisplay::internal(display_internal::TABLE_ROW)),
            AnonymousStyleKind::TableCell | AnonymousStyleKind::MissingTableCell => {
                style.set_display(FfiDisplay::internal(display_internal::TABLE_CELL));
            }
            AnonymousStyleKind::Table => style.set_display(FfiDisplay::outside_and_inside(
                display_outside::BLOCK,
                display_inside::TABLE,
                false,
            )),
            AnonymousStyleKind::InlineTable => style.set_display(FfiDisplay::outside_and_inside(
                display_outside::INLINE,
                display_inside::TABLE,
                false,
            )),
            AnonymousStyleKind::TableWrapper => style.transfer_table_properties(&Self::from_record(engine, parent)),
            AnonymousStyleKind::ButtonFlexWrapper => {
                style.set_display(FfiDisplay::outside_and_inside(
                    display_outside::BLOCK,
                    display_inside::FLEX,
                    false,
                ));
                style.edit::<AlignmentValues>(|values| {
                    values.justify_content = justify_content::CENTER;
                    values.flex_direction = flex_direction::COLUMN;
                });
                style.edit::<SizingValues>(|values| {
                    values.height = ComputedSize::new(
                        ComputedSizeKind::Percentage,
                        ComputedStyleValueHandle::percentage(100.0),
                    );
                });
            }
            AnonymousStyleKind::FieldsetContentWrapper => {
                let source = Self::from_record(engine, parent);
                style.set_display(FfiDisplay::outside_and_inside(
                    display_outside::BLOCK,
                    if source.group::<BoxValues>().display.is_flex_inside() {
                        display_inside::FLEX
                    } else {
                        display_inside::FLOW_ROOT
                    },
                    false,
                ));
                style.edit::<AlignmentValues>(|target| {
                    crate::css::computed_values::copy_fieldset_content_alignment(
                        source.group::<AlignmentValues>(),
                        target,
                    );
                });
                style.set_overflow(overrides.overflow_x, overrides.overflow_y);
            }
            AnonymousStyleKind::InlineStyleWrapper | AnonymousStyleKind::WritingSuggestion => style.set_display(
                FfiDisplay::outside_and_inside(display_outside::INLINE, display_inside::FLOW, false),
            ),
        }
        if kind == AnonymousStyleKind::WritingSuggestion {
            style.fade_text_color();
        }
        if kind == AnonymousStyleKind::MissingTableCell {
            style.edit::<BoxValues>(|values| {
                values.vertical_align = ComputedVerticalAlign {
                    is_keyword: true,
                    keyword: vertical_align::MIDDLE,
                    value: ComputedStyleValueHandle::empty(),
                }
            });
        }
        if overrides.columns_from_parent {
            style.copy_column_properties(&Self::from_record(engine, parent));
        }
        if kind == AnonymousStyleKind::ButtonContentBox {
            style.edit::<SizingValues>(|values| {
                values.min_height = ComputedSize::new(ComputedSizeKind::Length, ComputedStyleValueHandle::length(0.0));
            });
        }
        style
    }

    pub(crate) fn intern(self, engine: &mut StyleEngine) -> DerivedStyleRecord {
        // A layout style holds the longhands of the record it was made from, or the initial ones
        // where it only inherits its parent's groups.
        let record = match self.longhand_source {
            Some(source) => engine
                .install_record_derived_from(
                    super::computed::FinalStyleRecordID::from_raw(source).expect("a layout style's source is a record"),
                    &self.payloads,
                )
                .expect("a layout style's source record is one the engine installed"),
            None => engine.install_initial_derived_record(&self.payloads, 0),
        };
        DerivedStyleRecord::pin(engine, record.raw())
    }
}

impl StyleEngine {
    /// Has the installation of a style update's rows in the boxes of their elements remember the record each
    /// anonymous box it styles again derives, until [`Self::end_reinheriting_for_install`]: the anonymous boxes under
    /// elements styled alike derive the same record, which is then found rather than built and interned again.
    pub(crate) fn begin_reinheriting_for_install(&mut self) {
        debug_assert!(self.host.reinherited_records.is_none());
        self.host.reinherited_records = Some(Default::default());
    }

    /// Ends what [`Self::begin_reinheriting_for_install`] began, letting go of the records it remembered.
    pub(crate) fn end_reinheriting_for_install(&mut self) {
        for (_, record) in self.host.reinherited_records.take().unwrap_or_default() {
            self.unpin_layout_style_record(record);
        }
    }

    /// The record an anonymous box holding `record` derives from `parent`, its parent's, taking the column properties
    /// of `column_source` where it has one, pinned for the box.
    pub(crate) fn reinherited_anonymous_style_record(
        &mut self,
        record: u64,
        parent: u64,
        column_source: Option<u64>,
    ) -> DerivedStyleRecord {
        let key = Reinheritance {
            record,
            parent,
            column_source,
        };
        if let Some(&derived) = self
            .host
            .reinherited_records
            .as_ref()
            .and_then(|records| records.get(&key))
        {
            return DerivedStyleRecord::pin(self, derived);
        }
        let mut style = LayoutStyle::from_record(self, record);
        style.inherit_from(self, parent);
        if let Some(column_source) = column_source {
            style.copy_column_properties(&LayoutStyle::from_record(self, column_source));
        }
        let derived = style.intern(self);
        // NB: What is remembered stays pinned until the installation ends, as a box that took it may let go of it.
        if let Some(records) = self.host.reinherited_records.as_mut() {
            records.insert(key, derived.record);
            self.pin_layout_style_record(derived.record);
        }
        derived
    }

    pub(crate) fn pin_layout_style_record(&mut self, record: u64) {
        self.pin_style_record(record);
    }

    pub(crate) fn unpin_layout_style_record(&mut self, record: u64) {
        self.unpin_style_record(record);
    }
}
