/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Layout styles share the style engine's immutable records. Derivation edits only the
//! affected groups, and publishes final animated payloads as an independent base record.

use super::StyleEngine;
use crate::css::computed_longhand_table::ComputedLonghandTable;
use crate::css::computed_value_types::*;
use crate::css::computed_values::{
    InheritedBoxValues, clone_group_payload, default_group_payload, release_group_payload, retain_group_payload,
};
use crate::css::css_enums::{
    display_inside, display_internal, display_outside, flex_direction, justify_content, vertical_align,
};
use crate::css::display::FfiDisplay;
use crate::css::table_group_builder::group_index;
use std::ffi::c_void;

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
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct AnonymousStyleOverrides {
    pub inline_block_wrapper: bool,
    pub overflow_x: u8,
    pub overflow_y: u8,
}

#[derive(Clone, Copy)]
pub(crate) struct DerivedStyleRecord {
    pub record: u64,
    pub payloads: *const c_void,
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
    InheritedBoxValues => INHERITED_BOX,
    AlignmentValues => ALIGNMENT,
    SurroundValues => SURROUND,
    SizingValues => SIZING,
    GridValues => GRID,
    MiscResetValues => MISC_RESET,
    EffectsValues => EFFECTS,
    AnchorValues => ANCHOR,
}

pub(crate) struct LayoutStyle {
    payloads: [*const c_void; group_index::COUNT],
    longhand_table: *const ComputedLonghandTable,
    changed: bool,
}

impl Drop for LayoutStyle {
    fn drop(&mut self) {
        for (index, payload) in self.payloads.iter().copied().enumerate() {
            release_group_payload(index, payload);
        }
        if !self.longhand_table.is_null() {
            // SAFETY: The constructor retained this frozen table for the builder.
            unsafe {
                crate::css::computed_longhand_table::rust_computed_longhand_table_release(
                    self.longhand_table.cast_mut(),
                );
            };
        }
    }
}

impl LayoutStyle {
    pub(crate) fn from_record(engine: &StyleEngine, record: u64) -> Self {
        let view = engine
            .style_record_view(record)
            .expect("layout style record must be live");
        let payloads = view
            .payloads
            .try_into()
            .expect("layout styles have every computed group");
        let mut style = Self {
            payloads,
            longhand_table: view.longhand_table,
            changed: false,
        };
        for (index, payload) in style.payloads.iter().copied().enumerate() {
            retain_group_payload(index, payload);
        }
        if !style.longhand_table.is_null() {
            // SAFETY: The record owns a live, frozen table.
            style.longhand_table = unsafe {
                crate::css::computed_longhand_table::rust_computed_longhand_table_retain(
                    style.longhand_table.cast_mut(),
                )
            };
        }
        style
    }

    fn inherited(engine: &StyleEngine, parent: u64) -> Self {
        let parent = engine
            .style_record_payloads(parent)
            .expect("anonymous box parent style must be live");
        let payloads = std::array::from_fn(|index| {
            let payload = if index < super::computed::ENGINE_INHERITED_GROUP_COUNT {
                parent[index]
            } else {
                default_group_payload(index)
            };
            retain_group_payload(index, payload);
            payload
        });
        Self {
            payloads,
            longhand_table: std::ptr::null(),
            changed: false,
        }
    }

    pub(crate) fn inherit_from(&mut self, engine: &StyleEngine, parent: u64) {
        let parent = engine
            .style_record_payloads(parent)
            .expect("anonymous box parent style must be live");
        for (index, payload) in parent
            .iter()
            .copied()
            .enumerate()
            .take(super::computed::ENGINE_INHERITED_GROUP_COUNT)
        {
            retain_group_payload(index, payload);
            release_group_payload(index, std::mem::replace(&mut self.payloads[index], payload));
        }
    }

    fn group<T: LayoutStyleGroup>(&self) -> &T {
        // SAFETY: The private trait pairs each type with its registered group index.
        unsafe { &*self.payloads[T::INDEX].cast::<T>() }
    }

    fn edit<T: LayoutStyleGroup>(&mut self, update: impl FnOnce(&mut T)) {
        let mut value = self.group::<T>().clone();
        update(&mut value);
        if value == *self.group::<T>() {
            return;
        }
        self.changed = true;
        // SAFETY: The clone is uniquely owned and has the group type named by T.
        let payload = unsafe { clone_group_payload(T::INDEX, self.payloads[T::INDEX]) };
        unsafe { *payload.cast::<T>() = value };
        release_group_payload(T::INDEX, std::mem::replace(&mut self.payloads[T::INDEX], payload));
    }

    pub(crate) fn is_unchanged(&self) -> bool {
        !self.changed
    }

    pub(crate) fn set_display(&mut self, display: FfiDisplay) {
        self.edit::<BoxValues>(|values| values.display = display);
    }

    pub(crate) fn set_overflow(&mut self, x: u8, y: u8) {
        self.edit::<BoxValues>(|values| {
            values.overflow_x = x;
            values.overflow_y = y;
        });
    }

    pub(crate) fn set_writing_mode_and_direction(&mut self, writing_mode: u8, direction: u8) {
        self.edit::<InheritedBoxValues>(|values| {
            values.writing_mode = writing_mode;
            values.direction = direction;
        });
    }

    pub(crate) fn set_scrollbar_width(&mut self, width: u8) {
        self.edit::<MiscResetValues>(|values| values.scrollbar_width = width);
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
            target.position = box_values.position;
            target.float_ = box_values.float_;
            target.clear = box_values.clear;
            target.has_z_index = box_values.has_z_index;
            target.z_index = box_values.z_index;
            target.vertical_align = box_values.vertical_align.clone();
        });
        self.copy_table_properties(source);
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

    pub(crate) fn reset_table_properties(&mut self) {
        let defaults = Self {
            payloads: std::array::from_fn(|index| {
                let payload = default_group_payload(index);
                retain_group_payload(index, payload);
                payload
            }),
            longhand_table: std::ptr::null(),
            changed: false,
        };
        let initial = defaults.group::<BoxValues>();
        self.edit::<BoxValues>(|target| {
            target.position = initial.position;
            target.float_ = initial.float_;
            target.clear = initial.clear;
            target.has_z_index = initial.has_z_index;
            target.z_index = initial.z_index;
            target.vertical_align = initial.vertical_align.clone();
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
        let block_flow = FfiDisplay::outside_and_inside(
            if overrides.inline_block_wrapper {
                display_outside::INLINE
            } else {
                display_outside::BLOCK
            },
            if overrides.inline_block_wrapper {
                display_inside::FLOW_ROOT
            } else {
                display_inside::FLOW
            },
            false,
        );
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
                    values.height = ComputedSize {
                        kind: ComputedSizeKind::Percentage,
                        value: ComputedStyleValueHandle::percentage(100.0),
                    }
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
            AnonymousStyleKind::InlineStyleWrapper => style.set_display(FfiDisplay::outside_and_inside(
                display_outside::INLINE,
                display_inside::FLOW,
                false,
            )),
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
        if kind == AnonymousStyleKind::ButtonContentBox {
            style.edit::<SizingValues>(|values| {
                values.min_height = ComputedSize {
                    kind: ComputedSizeKind::Length,
                    value: ComputedStyleValueHandle::length(0.0),
                }
            });
        }
        style
    }

    pub(crate) fn intern(self, engine: &mut StyleEngine) -> DerivedStyleRecord {
        // SAFETY: The builder retains its table until publication has retained it in the record.
        let table = unsafe { self.longhand_table.as_ref() };
        let record = super::bridge::publish_computed_groups_from_inputs(
            engine,
            0,
            u8::MAX,
            &self.payloads,
            super::computed::ENGINE_INHERITED_GROUP_COUNT,
            0,
            false,
            0,
            0,
            std::ptr::null(),
            &[],
            table,
            std::ptr::null(),
        )
        .new_style_record;
        engine.pin_layout_style_record(record);
        DerivedStyleRecord {
            record,
            payloads: engine
                .style_record_payloads(record)
                .expect("new layout style must be live")
                .as_ptr()
                .cast(),
        }
    }
}

impl StyleEngine {
    pub(crate) fn pin_layout_style_record(&mut self, record: u64) {
        self.pin_style_record(record);
        self.record_boundary_call(super::record_replay::EventKind::PinStyleRecord, |payload| {
            payload.write_u64(record);
        });
    }

    pub(crate) fn unpin_layout_style_record(&mut self, record: u64) {
        self.unpin_style_record(record);
        self.record_boundary_call(super::record_replay::EventKind::UnpinStyleRecord, |payload| {
            payload.write_u64(record);
        });
    }
}
