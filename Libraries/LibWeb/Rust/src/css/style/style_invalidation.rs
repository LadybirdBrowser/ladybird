/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::StyleEngine;
use super::bridge::{FfiAnimationInvalidation, FfiStyleInvalidationField};
use crate::css::animated_overlay::{AnimatedOverlay, overlay_wins};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::computed_values::style_group_payloads_equal;
use crate::css::property_metadata::{
    self, FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, NUMBER_OF_LONGHAND_PROPERTIES, property_id,
};
use crate::css::style_value::StyleValueData;

const INVALIDATION_REPAINT: u8 = 1;
const INVALIDATION_RELAYOUT: u8 = 2;
const INVALIDATION_REBUILD_LAYOUT_TREE: u8 = 3;
const VISUAL_CONTEXT_UPDATE_VALUES: u8 = 1;
const VISUAL_CONTEXT_REBUILD: u8 = 2;
const REBUILD_ROOT_SELF: u8 = 0;
const REBUILD_ROOT_SELF_UNLESS_DOCUMENT_ELEMENT_OR_BODY: u8 = 1;
const REBUILD_ROOT_BOX_PRESENCE_CHANGE: u8 = 2;
const REBUILD_ROOT_PARENT: u8 = 3;
const ALL_INHERITED_STYLE_GROUPS: u8 = (1 << 7) - 1;

#[derive(Clone, Copy, Default)]
struct StyleInvalidation {
    level: u8,
    visual_context: u8,
    rebuild_root: u8,
    rebuild_stacking_context: bool,
    recalculate_scrollable_overflow: bool,
    resnap_scroll_container: bool,
    recompute_descendants: bool,
    inherited_groups: u8,
    changes_containing_block: bool,
    repaint_text_decorations: bool,
    non_inherited_inheritance_source: bool,
    any_computed_value_changed: bool,
}

impl StyleInvalidation {
    fn is_none(self) -> bool {
        self.pack() == 0
    }

    fn ensure_level(&mut self, level: u8) {
        self.level = self.level.max(level);
        if level >= INVALIDATION_REBUILD_LAYOUT_TREE {
            self.rebuild_root = REBUILD_ROOT_PARENT;
        }
    }

    fn ensure_visual_context(&mut self, level: u8) {
        self.visual_context = self.visual_context.max(level);
    }

    fn rebuild_layout_tree_from(root: u8) -> Self {
        Self {
            level: INVALIDATION_REBUILD_LAYOUT_TREE,
            rebuild_root: root,
            rebuild_stacking_context: true,
            ..Self::default()
        }
    }

    fn full() -> Self {
        Self::rebuild_layout_tree_from(REBUILD_ROOT_PARENT)
    }

    fn merge(&mut self, other: Self) {
        if other.level >= INVALIDATION_REBUILD_LAYOUT_TREE {
            self.rebuild_root = if self.level >= INVALIDATION_REBUILD_LAYOUT_TREE {
                self.rebuild_root.max(other.rebuild_root)
            } else {
                other.rebuild_root
            };
        }
        self.level = self.level.max(other.level);
        self.visual_context = self.visual_context.max(other.visual_context);
        self.rebuild_stacking_context |= other.rebuild_stacking_context;
        self.recalculate_scrollable_overflow |= other.recalculate_scrollable_overflow;
        self.resnap_scroll_container |= other.resnap_scroll_container;
        self.recompute_descendants |= other.recompute_descendants;
        self.inherited_groups |= other.inherited_groups;
        self.changes_containing_block |= other.changes_containing_block;
        self.repaint_text_decorations |= other.repaint_text_decorations;
        self.non_inherited_inheritance_source |= other.non_inherited_inheritance_source;
        self.any_computed_value_changed |= other.any_computed_value_changed;
    }

    fn pack(self) -> u32 {
        let mut packed = u32::from(self.level) & FfiStyleInvalidationField::LevelMask as u32;
        packed |= u32::from(self.visual_context) << FfiStyleInvalidationField::VisualContextShift as u32;
        packed |= u32::from(self.rebuild_root) << FfiStyleInvalidationField::RebuildRootShift as u32;
        packed |= u32::from(self.rebuild_stacking_context) * FfiStyleInvalidationField::RebuildStackingContext as u32;
        packed |= u32::from(self.recalculate_scrollable_overflow)
            * FfiStyleInvalidationField::RecalculateScrollableOverflow as u32;
        packed |= u32::from(self.resnap_scroll_container) * FfiStyleInvalidationField::ResnapScrollContainer as u32;
        packed |= u32::from(self.recompute_descendants) * FfiStyleInvalidationField::RecomputeDescendants as u32;
        packed |= (u32::from(self.inherited_groups) & FfiStyleInvalidationField::InheritedGroupsMask as u32)
            << FfiStyleInvalidationField::InheritedGroupsShift as u32;
        packed |= u32::from(self.changes_containing_block) * FfiStyleInvalidationField::ChangesContainingBlock as u32;
        packed |= u32::from(self.repaint_text_decorations) * FfiStyleInvalidationField::RepaintTextDecorations as u32;
        packed |= u32::from(self.non_inherited_inheritance_source)
            * FfiStyleInvalidationField::NonInheritedInheritanceSource as u32;
        packed |=
            u32::from(self.any_computed_value_changed) * FfiStyleInvalidationField::AnyComputedValueChanged as u32;
        packed
    }
}

fn style_value_is_none(value: Option<&StyleValueData>) -> bool {
    match value {
        Some(StyleValueData::Keyword { keyword }) => *keyword == crate::css::css_enums::keyword::NONE,
        Some(StyleValueData::ValueList { values, .. }) => values
            .as_slice()
            .iter()
            .all(|value| style_value_is_none(Some(value.data()))),
        None => true,
        _ => false,
    }
}

fn value_creates_stacking_context(property: u16, values: ComputedValuesView<'_>) -> bool {
    let effects = values.effects();
    let transform = values.transform();
    let mask = values.mask();
    match property {
        property_id::OPACITY => effects.opacity < 1.0,
        property_id::TRANSFORM => !style_value_is_none(transform.transformations.data()),
        property_id::TRANSLATE => !style_value_is_none(transform.translate.data()),
        property_id::ROTATE => !style_value_is_none(transform.rotate.data()),
        property_id::SCALE => !style_value_is_none(transform.scale.data()),
        property_id::FILTER => !effects.filter.operations.as_slice().is_empty(),
        property_id::BACKDROP_FILTER => !effects.backdrop_filter.operations.as_slice().is_empty(),
        property_id::CLIP_PATH => !style_value_is_none(mask.clip_path.data()),
        property_id::MASK_IMAGE => !style_value_is_none(mask.mask_image.data()),
        property_id::VIEW_TRANSITION_NAME => !style_value_is_none(values.misc_reset().view_transition_name.data()),
        property_id::ISOLATION => effects.isolation == crate::css::css_enums::isolation::ISOLATE,
        property_id::MIX_BLEND_MODE => effects.mix_blend_mode != crate::css::css_enums::mix_blend_mode::NORMAL,
        property_id::Z_INDEX => values.box_values().has_z_index,
        property_id::PERSPECTIVE => transform.has_perspective,
        property_id::TRANSFORM_STYLE => transform.transform_style != crate::css::css_enums::transform_style::FLAT,
        property_id::BACKFACE_VISIBILITY => {
            transform.backface_visibility == crate::css::css_enums::backface_visibility::HIDDEN
        }
        property_id::CONTAIN => values.box_values().layout_containment || values.box_values().paint_containment,
        property_id::CONTAINER_TYPE => {
            values.box_values().is_size_container || values.box_values().is_inline_size_container
        }
        property_id::CONTENT_VISIBILITY => {
            values.content_visibility() == crate::css::css_enums::content_visibility::AUTO
        }
        property_id::WILL_CHANGE => will_change_creates_stacking_context(values.misc_reset().will_change.data()),
        _ => true,
    }
}

fn will_change_mentions(value: Option<&StyleValueData>, predicate: impl Fn(u16) -> bool) -> bool {
    let Some(StyleValueData::ValueList { values, .. }) = value else {
        return false;
    };
    values.as_slice().iter().any(|entry| {
        let StyleValueData::CustomIdent { custom_ident } = entry.data() else {
            return false;
        };
        crate::css::serialize::with_fly_string_units(custom_ident, |units| {
            let property = match units {
                crate::css::serialize::StringUnits::Ascii(bytes) => {
                    let units = bytes.iter().copied().map(u16::from).collect::<Vec<_>>();
                    property_metadata::property_id_from_name(&units)
                }
                crate::css::serialize::StringUnits::Utf16(units) => property_metadata::property_id_from_name(units),
            };
            property.is_some_and(&predicate)
        })
    })
}

fn will_change_creates_stacking_context(value: Option<&StyleValueData>) -> bool {
    will_change_mentions(value, |property| {
        matches!(
            property,
            property_id::OPACITY
                | property_id::TRANSFORM
                | property_id::TRANSLATE
                | property_id::ROTATE
                | property_id::SCALE
                | property_id::FILTER
                | property_id::BACKDROP_FILTER
                | property_id::CLIP_PATH
                | property_id::MASK
                | property_id::MASK_IMAGE
                | property_id::ISOLATION
                | property_id::MIX_BLEND_MODE
                | property_id::Z_INDEX
                | property_id::POSITION
                | property_id::PERSPECTIVE
                | property_id::TRANSFORM_STYLE
                | property_id::BACKFACE_VISIBILITY
                | property_id::CONTAIN
                | property_id::VIEW_TRANSITION_NAME
        )
    })
}

fn will_change_establishes_containing_block(value: Option<&StyleValueData>) -> bool {
    will_change_mentions(value, |property| {
        matches!(
            property,
            property_id::TRANSFORM
                | property_id::TRANSLATE
                | property_id::ROTATE
                | property_id::SCALE
                | property_id::PERSPECTIVE
                | property_id::TRANSFORM_STYLE
                | property_id::BACKFACE_VISIBILITY
                | property_id::FILTER
                | property_id::BACKDROP_FILTER
                | property_id::CONTAIN
                | property_id::POSITION
        )
    })
}

fn value_establishes_containing_block(property: u16, values: ComputedValuesView<'_>) -> bool {
    match property {
        property_id::TRANSFORM
        | property_id::TRANSLATE
        | property_id::ROTATE
        | property_id::SCALE
        | property_id::PERSPECTIVE
        | property_id::TRANSFORM_STYLE
        | property_id::BACKFACE_VISIBILITY
        | property_id::FILTER
        | property_id::BACKDROP_FILTER => value_creates_stacking_context(property, values),
        property_id::CONTAIN => values.box_values().layout_containment || values.box_values().paint_containment,
        property_id::WILL_CHANGE => will_change_establishes_containing_block(values.misc_reset().will_change.data()),
        property_id::CONTAINER_TYPE => {
            values.box_values().is_size_container || values.box_values().is_inline_size_container
        }
        _ => false,
    }
}

fn has_transform_style_grouping_property(values: ComputedValuesView<'_>) -> bool {
    let box_values = values.box_values();
    let effects = values.effects();
    let mask = values.mask();
    let overflow_groups = |overflow| {
        overflow != crate::css::css_enums::overflow::VISIBLE && overflow != crate::css::css_enums::overflow::CLIP
    };
    overflow_groups(box_values.overflow_x)
        || overflow_groups(box_values.overflow_y)
        || effects.opacity < 1.0
        || !effects.filter.operations.as_slice().is_empty()
        || effects.clip_is_rect
        || !style_value_is_none(mask.clip_path.data())
        || effects.isolation == crate::css::css_enums::isolation::ISOLATE
        || !style_value_is_none(mask.mask_image.data())
        || effects.mix_blend_mode != crate::css::css_enums::mix_blend_mode::NORMAL
        || !effects.backdrop_filter.operations.as_slice().is_empty()
}

fn transform_value_is_invertible(property: u16, values: ComputedValuesView<'_>) -> Option<bool> {
    let transform = values.transform();
    if property == property_id::SCALE {
        let value = transform.scale.data()?;
        if matches!(value, StyleValueData::Keyword { keyword } if *keyword == crate::css::css_enums::keyword::NONE) {
            return Some(true);
        }
        let StyleValueData::Transformation {
            transform_function,
            values,
            ..
        } = value
        else {
            return None;
        };
        let matrix = crate::css::table_group_builder::transformation_to_matrix(*transform_function, values.as_slice());
        let mut elements = [[0.0; 4]; 4];
        for (index, value) in matrix.into_iter().enumerate() {
            elements[index / 4][index % 4] = value;
        }
        return Some(libgfx_rust::FloatMatrix4x4 { elements }.is_invertible());
    }
    if property != property_id::TRANSFORM {
        return None;
    }
    let individual_transform_count = [
        transform.translate.data(),
        transform.rotate.data(),
        transform.scale.data(),
    ]
    .into_iter()
    .filter(|value| matches!(value, Some(StyleValueData::Transformation { .. })))
    .count();
    let entries = &transform.resolved_transforms.as_slice()[individual_transform_count..];
    if entries.iter().any(|entry| {
        entry.is_translate && (!entry.x_percentage.pointer.is_null() || !entry.y_percentage.pointer.is_null())
    }) {
        return None;
    }
    let mut matrix = libgfx_rust::FloatMatrix4x4::identity();
    for entry in entries {
        let mut elements = [[0.0; 4]; 4];
        for (index, value) in entry.matrix.iter().copied().enumerate() {
            elements[index / 4][index % 4] = value;
        }
        matrix = matrix.multiplied(libgfx_rust::FloatMatrix4x4 { elements });
    }
    Some(matrix.is_invertible())
}

fn accumulated_visual_context_property_always_requires_repaint(property: u16) -> bool {
    matches!(
        property,
        property_id::BACKDROP_FILTER
            | property_id::BACKGROUND_ATTACHMENT
            | property_id::BACKGROUND_IMAGE
            | property_id::BORDER_BOTTOM_LEFT_RADIUS
            | property_id::BORDER_BOTTOM_RIGHT_RADIUS
            | property_id::BORDER_TOP_LEFT_RADIUS
            | property_id::BORDER_TOP_RIGHT_RADIUS
            | property_id::MASK_IMAGE
            | property_id::MASK_TYPE
            | property_id::MIX_BLEND_MODE
            | property_id::PERSPECTIVE
    )
}

fn clip_path_value_is_a_visual_context_frame(values: ComputedValuesView<'_>) -> bool {
    matches!(values.mask().clip_path.data(), Some(StyleValueData::BasicShape { .. }))
}

fn accumulated_visual_context_change_requires_repaint(
    property: u16,
    old: ComputedValuesView<'_>,
    new: ComputedValuesView<'_>,
) -> bool {
    if property == property_id::OPACITY && (old.effects().opacity == 0.0) != (new.effects().opacity == 0.0) {
        return true;
    }
    if matches!(property, property_id::TRANSFORM | property_id::SCALE) {
        let old_invertible = transform_value_is_invertible(property, old);
        let new_invertible = transform_value_is_invertible(property, new);
        if old_invertible.is_none() || new_invertible.is_none() || old_invertible != new_invertible {
            return true;
        }
    }
    if property == property_id::CLIP_PATH {
        return !clip_path_value_is_a_visual_context_frame(old) || !clip_path_value_is_a_visual_context_frame(new);
    }
    if property == property_id::CLIP {
        return !old.effects().clip_is_rect || !new.effects().clip_is_rect;
    }
    accumulated_visual_context_property_always_requires_repaint(property)
}

fn property_invalidation(property: u16, old: ComputedValuesView<'_>, new: ComputedValuesView<'_>) -> StyleInvalidation {
    let old_display = old.display();
    let new_display = new.display();
    if property == property_id::POSITION && old.is_absolutely_positioned() != new.is_absolutely_positioned() {
        return StyleInvalidation::full();
    }
    if property == property_id::FLOAT && old.is_floating() != new.is_floating() {
        return StyleInvalidation::full();
    }
    if matches!(
        property,
        property_id::DISPLAY | property_id::FLOAT | property_id::POSITION
    ) && old_display.is_outside_and_inside()
        && new_display.is_outside_and_inside()
        && old_display.outside == new_display.outside
    {
        return StyleInvalidation::rebuild_layout_tree_from(REBUILD_ROOT_SELF);
    }
    if matches!(
        property,
        property_id::CONTENT | property_id::CONTENT_VISIBILITY | property_id::TEXT_TRANSFORM
    ) {
        return StyleInvalidation::rebuild_layout_tree_from(REBUILD_ROOT_SELF);
    }
    if matches!(
        property,
        property_id::LIST_STYLE_TYPE | property_id::LIST_STYLE_IMAGE | property_id::LIST_STYLE_POSITION
    ) && (old_display.is_list_item() || new_display.is_list_item())
    {
        return StyleInvalidation::rebuild_layout_tree_from(REBUILD_ROOT_SELF);
    }
    if property == property_id::DISPLAY && old_display.is_none() != new_display.is_none() {
        let box_display = if old_display.is_none() {
            new_display
        } else {
            old_display
        };
        if box_display.is_outside_and_inside() {
            return StyleInvalidation::rebuild_layout_tree_from(REBUILD_ROOT_BOX_PRESENCE_CHANGE);
        }
    }
    if matches!(
        property,
        property_id::DISPLAY | property_id::FLOAT | property_id::POSITION
    ) {
        return StyleInvalidation::full();
    }
    if matches!(property, property_id::OVERFLOW_X | property_id::OVERFLOW_Y) {
        return StyleInvalidation::rebuild_layout_tree_from(REBUILD_ROOT_SELF_UNLESS_DOCUMENT_ELEMENT_OR_BODY);
    }
    if matches!(
        property,
        property_id::COUNTER_RESET | property_id::COUNTER_SET | property_id::COUNTER_INCREMENT
    ) {
        let mut result = StyleInvalidation::default();
        result.ensure_level(INVALIDATION_REBUILD_LAYOUT_TREE);
        return result;
    }

    let mut result = StyleInvalidation::default();
    if matches!(property, property_id::CONTAINER_NAME | property_id::CONTAINER_TYPE) {
        result.recompute_descendants = true;
    }
    if property == property_id::TEXT_DECORATION_LINE {
        result.repaint_text_decorations = true;
    } else if matches!(
        property,
        property_id::TEXT_DECORATION_COLOR
            | property_id::TEXT_DECORATION_STYLE
            | property_id::TEXT_DECORATION_THICKNESS
            | property_id::TEXT_UNDERLINE_OFFSET
            | property_id::TEXT_UNDERLINE_POSITION
            | property_id::COLOR
    ) && (!old.text_reset().text_decoration_lines.as_slice().is_empty()
        || !new.text_reset().text_decoration_lines.as_slice().is_empty())
    {
        result.repaint_text_decorations = property != property_id::COLOR
            || old.text_reset().text_decoration_color != new.text_reset().text_decoration_color;
    }
    if matches!(property, property_id::DIRECTION | property_id::WRITING_MODE) {
        result.recompute_descendants = true;
    }
    if property == property_id::VISIBILITY {
        let collapse = crate::css::css_enums::visibility::COLLAPSE;
        if (old.visibility() == collapse) != (new.visibility() == collapse) {
            result.ensure_level(INVALIDATION_RELAYOUT);
        }
        result.ensure_level(INVALIDATION_REPAINT);
    } else if property_metadata::property_affects_layout(property) {
        result.ensure_level(INVALIDATION_RELAYOUT);
    }
    if property_metadata::property_affects_scrollable_overflow(property) {
        result.recalculate_scrollable_overflow = true;
    }
    if property_metadata::property_affects_scrollable_overflow(property)
        || matches!(
            property,
            property_id::SCROLL_SNAP_TYPE
                | property_id::SCROLL_SNAP_ALIGN
                | property_id::SCROLL_SNAP_STOP
                | property_id::SCROLL_MARGIN_TOP
                | property_id::SCROLL_MARGIN_RIGHT
                | property_id::SCROLL_MARGIN_BOTTOM
                | property_id::SCROLL_MARGIN_LEFT
                | property_id::SCROLL_PADDING_TOP
                | property_id::SCROLL_PADDING_RIGHT
                | property_id::SCROLL_PADDING_BOTTOM
                | property_id::SCROLL_PADDING_LEFT
        )
    {
        result.resnap_scroll_container = true;
    }
    if property_metadata::property_affects_stacking_context(property)
        && (property == property_id::Z_INDEX
            || value_creates_stacking_context(property, old) != value_creates_stacking_context(property, new))
    {
        result.rebuild_stacking_context = true;
        result.ensure_level(INVALIDATION_REPAINT);
    }
    if value_establishes_containing_block(property, old) != value_establishes_containing_block(property, new) {
        result.changes_containing_block = true;
    }
    if new.transform().transform_style == crate::css::css_enums::transform_style::PRESERVE_3D
        && has_transform_style_grouping_property(old) != has_transform_style_grouping_property(new)
    {
        result.changes_containing_block = true;
        result.ensure_level(INVALIDATION_RELAYOUT);
    }
    let mut needs_repaint = true;
    if property_metadata::property_affects_accumulated_visual_contexts(property) {
        let value_only = matches!(
            property,
            property_id::TRANSFORM_ORIGIN | property_id::TRANSFORM_BOX | property_id::PERSPECTIVE_ORIGIN
        ) || (matches!(
            property,
            property_id::TRANSFORM
                | property_id::TRANSLATE
                | property_id::ROTATE
                | property_id::SCALE
                | property_id::OPACITY
                | property_id::FILTER
                | property_id::MIX_BLEND_MODE
                | property_id::PERSPECTIVE
        ) && value_creates_stacking_context(property, old)
            && value_creates_stacking_context(property, new));
        result.ensure_visual_context(if value_only {
            VISUAL_CONTEXT_UPDATE_VALUES
        } else {
            VISUAL_CONTEXT_REBUILD
        });
        if !accumulated_visual_context_change_requires_repaint(property, old, new)
            && result.level < INVALIDATION_REPAINT
            && !result.recompute_descendants
        {
            needs_repaint = false;
        }
    }
    if needs_repaint {
        result.ensure_level(INVALIDATION_REPAINT);
    }
    result
}

fn effective_value<'a>(view: &super::computed::StyleRecordView<'a>, property: u16) -> &'a StyleValueData {
    let index = usize::from(property - FIRST_LONGHAND_PROPERTY_ID);
    let table = unsafe { &*view.longhand_table };
    if let Some(entry) = unsafe { view.animated_overlay.as_ref() }.and_then(|overlay| overlay.get(property))
        && overlay_wins(entry, table.is_important(property))
    {
        return entry.value();
    }
    unsafe { &*view.longhand_values[index].cast::<StyleValueData>() }
}

fn effective_value_with_overlay(
    view: &super::computed::StyleRecordView<'_>,
    overlay: Option<&AnimatedOverlay>,
    property: u16,
) -> *const StyleValueData {
    let index = usize::from(property - FIRST_LONGHAND_PROPERTY_ID);
    let table = unsafe { &*view.longhand_table };
    if let Some(entry) = overlay.and_then(|overlay| overlay.get(property))
        && overlay_wins(entry, table.is_important(property))
    {
        return entry.value();
    }
    view.longhand_values[index].cast()
}

fn animation_overlay_properties<'a>(
    old_overlay: Option<&'a AnimatedOverlay>,
    new_overlay: Option<&'a AnimatedOverlay>,
) -> impl Iterator<Item = u16> + 'a {
    old_overlay
        .into_iter()
        .flat_map(AnimatedOverlay::entries)
        .map(|entry| entry.property)
        .chain(
            new_overlay
                .into_iter()
                .flat_map(AnimatedOverlay::entries)
                .filter(move |entry| old_overlay.is_none_or(|overlay| overlay.get(entry.property).is_none()))
                .map(|entry| entry.property),
        )
}

fn animation_value_changed(
    record: &super::computed::StyleRecordView<'_>,
    old_overlay: Option<&AnimatedOverlay>,
    new_overlay: Option<&AnimatedOverlay>,
    property: u16,
) -> bool {
    let old = effective_value_with_overlay(record, old_overlay, property);
    let new = effective_value_with_overlay(record, new_overlay, property);
    old != new && unsafe { *old != *new }
}

fn inheritance_dependent_values_equal(
    old: &crate::css::computed_longhand_table::ComputedLonghandTable,
    new: &crate::css::computed_longhand_table::ComputedLonghandTable,
) -> bool {
    let old = old.inheritance_dependent_values().collect::<Vec<_>>();
    let new = new.inheritance_dependent_values().collect::<Vec<_>>();
    old.len() == new.len()
        && old.iter().all(|(property, old_value)| {
            new.iter()
                .find(|(candidate, _)| candidate == property)
                .is_some_and(|(_, new_value)| {
                    old_value == new_value
                        || unsafe { *old_value.cast::<StyleValueData>() == *new_value.cast::<StyleValueData>() }
                })
        })
}

impl StyleEngine {
    pub(crate) fn animation_overlay_changed(
        &self,
        old_style_record: u64,
        animated_overlay: *const AnimatedOverlay,
    ) -> bool {
        let old_record = self
            .computed_group_sets
            .style_record_view(old_style_record)
            .unwrap_or_else(|| panic!("old style record {old_style_record:#x} is not live"));
        let old_overlay = unsafe { old_record.animated_overlay.as_ref() };
        let new_overlay = unsafe { animated_overlay.as_ref() };
        animation_overlay_properties(old_overlay, new_overlay)
            .any(|property| animation_value_changed(&old_record, old_overlay, new_overlay, property))
    }

    pub(crate) fn compare_animation_overlay(
        &self,
        old_style_record: u64,
        animated_overlay: *const AnimatedOverlay,
        payloads: &[*const std::ffi::c_void],
        is_document_element: bool,
    ) -> FfiAnimationInvalidation {
        let old_record = self
            .computed_group_sets
            .style_record_view(old_style_record)
            .unwrap_or_else(|| panic!("old style record {old_style_record:#x} is not live"));
        assert_eq!(payloads.len(), old_record.payloads.len());
        let old_overlay = unsafe { old_record.animated_overlay.as_ref() };
        let new_overlay = unsafe { animated_overlay.as_ref() };
        let old_values = ComputedValuesView::new(old_record.payloads);
        let new_values = ComputedValuesView::new(payloads);
        let mut ffi_result = FfiAnimationInvalidation::default();
        let mut invalidation = StyleInvalidation::default();
        let mut text_decoration_line_animated = false;

        for property in animation_overlay_properties(old_overlay, new_overlay) {
            if !animation_value_changed(&old_record, old_overlay, new_overlay, property) {
                continue;
            }
            if matches!(
                property,
                property_id::DIRECTION
                    | property_id::DISPLAY
                    | property_id::FLOAT
                    | property_id::OVERFLOW_X
                    | property_id::OVERFLOW_Y
                    | property_id::POSITION
                    | property_id::TEXT_ALIGN
            ) {
                ffi_result.requires_base_style_recomputation = true;
            }
            if property_metadata::property_is_inherited(property) {
                ffi_result.requires_layout_node_style_application = true;
            } else {
                ffi_result.changed_non_inherited_style_groups |=
                    property_metadata::property_style_group_index(property)
                        .map_or((1 << payloads.len()) - 1, |group| 1 << group);
            }
            if matches!(
                property,
                property_id::BACKGROUND_IMAGE | property_id::BORDER_IMAGE_SOURCE | property_id::MASK_IMAGE
            ) {
                ffi_result.requires_style_resource_update = true;
            }

            let mut property_damage = property_invalidation(property, old_values, new_values);
            if property == property_id::BACKGROUND_COLOR && is_document_element {
                property_damage.ensure_visual_context(VISUAL_CONTEXT_REBUILD);
            }
            if !property_damage.is_none() && property_metadata::property_is_inherited(property) {
                match property_metadata::property_style_group_index(property) {
                    Some(group) if group < 7 => property_damage.inherited_groups |= 1 << group,
                    _ => property_damage.inherited_groups = ALL_INHERITED_STYLE_GROUPS,
                }
            }
            if property == property_id::TEXT_DECORATION_LINE {
                text_decoration_line_animated = true;
            }
            invalidation.merge(property_damage);
        }

        // Animated properties other than text-decoration-line cannot make an undecorated box decorated.
        if invalidation.repaint_text_decorations
            && !text_decoration_line_animated
            && old_values.text_reset().text_decoration_lines.as_slice().is_empty()
        {
            invalidation.repaint_text_decorations = false;
        }
        ffi_result.invalidation = invalidation.pack();
        ffi_result
    }

    pub(crate) fn compare_style_records(
        &mut self,
        old_style_record: u64,
        new_style_record: u64,
        font_lists_equal: bool,
        element_folds_transform_into_layout: bool,
    ) -> u32 {
        let key = (
            old_style_record,
            new_style_record,
            font_lists_equal,
            element_folds_transform_into_layout,
        );
        if let Some(result) = self.style_invalidation_cache.get(&key) {
            return *result | FfiStyleInvalidationField::CacheHit as u32;
        }
        let old_record = self
            .computed_group_sets
            .style_record_view(old_style_record)
            .unwrap_or_else(|| panic!("old style record {old_style_record:#x} is not live"));
        let new_record = self
            .computed_group_sets
            .style_record_view(new_style_record)
            .unwrap_or_else(|| panic!("new style record {new_style_record:#x} is not live"));
        let old_values = ComputedValuesView::new(old_record.payloads);
        let new_values = ComputedValuesView::new(new_record.payloads);
        let old_table = unsafe { &*old_record.longhand_table };
        let new_table = unsafe { &*new_record.longhand_table };
        let all_groups_equal = old_record
            .payloads
            .iter()
            .zip(new_record.payloads)
            .enumerate()
            .all(|(index, (&old, &new))| old == new || style_group_payloads_equal(index, old, new));
        let can_skip = all_groups_equal
            && std::ptr::eq(old_table, new_table)
            && font_lists_equal
            && old_record.animated_overlay.is_null()
            && new_record.animated_overlay.is_null()
            && inheritance_dependent_values_equal(old_table, new_table);
        let mut result = StyleInvalidation::default();
        if !font_lists_equal {
            result.any_computed_value_changed = true;
            result.ensure_level(INVALIDATION_RELAYOUT);
        }
        if !can_skip {
            let old_writing_mode = old_values.writing_mode();
            let old_direction = old_values.direction();
            let new_writing_mode = new_values.writing_mode();
            let new_direction = new_values.direction();
            let mut effective_changed = [false; NUMBER_OF_LONGHAND_PROPERTIES];
            for (index, changed) in effective_changed.iter_mut().enumerate() {
                let property = FIRST_LONGHAND_PROPERTY_ID + index as u16;
                let old_physical = if property_metadata::longhand_is_logical_alias(property) {
                    crate::css::style_compute::map_logical_alias_to_physical(property, old_writing_mode, old_direction)
                } else {
                    property
                };
                let new_physical = if property_metadata::longhand_is_logical_alias(property) {
                    crate::css::style_compute::map_logical_alias_to_physical(property, new_writing_mode, new_direction)
                } else {
                    property
                };
                let old_value = effective_value(&old_record, old_physical);
                let new_value = effective_value(&new_record, new_physical);
                if std::ptr::eq(old_value, new_value) || old_value == new_value {
                    continue;
                }
                *changed = true;
                result.any_computed_value_changed = true;
                if property_metadata::property_is_inherited(property) {
                    match property_metadata::property_style_group_index(new_physical) {
                        Some(group) if group < 7 => result.inherited_groups |= 1 << group,
                        _ => result.inherited_groups = ALL_INHERITED_STYLE_GROUPS,
                    }
                }
                let mut invalidation = property_invalidation(property, old_values, new_values);
                if element_folds_transform_into_layout
                    && matches!(
                        property,
                        property_id::TRANSFORM | property_id::TRANSLATE | property_id::ROTATE | property_id::SCALE
                    )
                {
                    invalidation.ensure_level(INVALIDATION_RELAYOUT);
                }
                result.merge(invalidation);
            }
            let old_overlay = unsafe { old_record.animated_overlay.cast::<AnimatedOverlay>().as_ref() };
            let new_overlay = unsafe { new_record.animated_overlay.cast::<AnimatedOverlay>().as_ref() };
            if old_overlay.is_some() || new_overlay.is_some() {
                for (index, effective_changed) in effective_changed.into_iter().enumerate() {
                    if effective_changed {
                        continue;
                    }
                    let property = FIRST_LONGHAND_PROPERTY_ID + index as u16;
                    if old_overlay.and_then(|overlay| overlay.get(property)).is_none()
                        && new_overlay.and_then(|overlay| overlay.get(property)).is_none()
                    {
                        continue;
                    }
                    let old_physical = crate::css::style_compute::map_logical_alias_to_physical(
                        property,
                        old_writing_mode,
                        old_direction,
                    );
                    let new_physical = crate::css::style_compute::map_logical_alias_to_physical(
                        property,
                        new_writing_mode,
                        new_direction,
                    );
                    let old_base = unsafe {
                        &*old_record.longhand_values[usize::from(old_physical - FIRST_LONGHAND_PROPERTY_ID)]
                            .cast::<StyleValueData>()
                    };
                    let new_base = unsafe {
                        &*new_record.longhand_values[usize::from(new_physical - FIRST_LONGHAND_PROPERTY_ID)]
                            .cast::<StyleValueData>()
                    };
                    if std::ptr::eq(old_base, new_base) || old_base == new_base {
                        continue;
                    }
                    result.any_computed_value_changed = true;
                    if !property_metadata::property_is_inherited(property) {
                        result.non_inherited_inheritance_source = true;
                    } else {
                        match property_metadata::property_style_group_index(new_physical) {
                            Some(group) if group < 7 => result.inherited_groups |= 1 << group,
                            _ => result.inherited_groups = ALL_INHERITED_STYLE_GROUPS,
                        }
                    }
                }
            }
        }
        let packed = result.pack();
        if self.style_invalidation_cache.len() >= 4096 {
            self.style_invalidation_cache.clear();
        }
        self.style_invalidation_cache.insert(key, packed);
        packed
    }
}

const _: () = assert!(FIRST_LONGHAND_PROPERTY_ID <= LAST_LONGHAND_PROPERTY_ID);
