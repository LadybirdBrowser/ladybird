/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::bridge::{ElementBoxKind, FfiAnimationInvalidation, FfiStyleInvalidationField, element_adjustment_fact};
use super::{RetainedState, StyleNodeID};
use crate::css::animated_overlay::{AnimatedOverlay, overlay_wins};
use crate::css::computed_longhand_table::ComputedLonghandTable;
use crate::css::computed_value_types::SVG_PAINT_NONE;
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::computed_values::style_group_payloads_equal;
use crate::css::host_shared::SharedPayload;
use crate::css::property_metadata::{
    self, FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, NUMBER_OF_LONGHAND_PROPERTIES, property_id,
};
use crate::css::style_value::StyleValueData;

const INVALIDATION_REPAINT: u8 = 1;
const INVALIDATION_RELAYOUT: u8 = 2;
const INVALIDATION_REBUILD_LAYOUT_TREE: u8 = 3;
const VISUAL_CONTEXT_UPDATE_VALUES: u8 = 1;
const VISUAL_CONTEXT_REBUILD: u8 = 2;
const REBUILD_ROOT_PSEUDO_ELEMENTS: u8 = 0;
const REBUILD_ROOT_SELF: u8 = 1;
const REBUILD_ROOT_BOX_PRESENCE_CHANGE: u8 = 3;
const REBUILD_ROOT_PARENT: u8 = 4;
const ALL_INHERITED_STYLE_GROUPS: u8 = (1 << 7) - 1;

/// What a sample may change beside the box of its element: the visual contexts the box takes part in, which the render
/// owner builds again for the frames it records, and where no scroll container of the document snaps, which
/// `scroll_snaps` says, the scrollable overflow of its scroll container. The values the element's children inherit
/// follow, as the caller composes them over the children itself, and the text decorations its subtree draws where
/// `subtree_follows` says the caller repaints every element in it.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct SampleBounds {
    pub scroll_snaps: bool,
    pub subtree_follows: bool,
}

#[derive(Clone, Copy, Default)]
struct StyleInvalidation {
    level: u8,
    visual_context: u8,
    rebuild_root: u8,
    rebuild_stacking_context: bool,
    resnap_scroll_container: bool,
    recompute_descendants: bool,
    inherited_groups: u8,
    repaint_text_decorations: bool,
    non_inherited_inheritance_source: bool,
    any_computed_value_changed: bool,
    affects_hit_testing: bool,
    repaint_highlights: bool,
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
        self.resnap_scroll_container |= other.resnap_scroll_container;
        self.recompute_descendants |= other.recompute_descendants;
        self.inherited_groups |= other.inherited_groups;
        self.repaint_text_decorations |= other.repaint_text_decorations;
        self.non_inherited_inheritance_source |= other.non_inherited_inheritance_source;
        self.any_computed_value_changed |= other.any_computed_value_changed;
        self.affects_hit_testing |= other.affects_hit_testing;
        self.repaint_highlights |= other.repaint_highlights;
    }

    /// Whether the damage stays in the box of its element: no layout tree, stacking context, visual context, scroll
    /// snap or text decoration of descendants.
    fn stays_in_its_box(self) -> bool {
        self.stays_in_its_box_and_visual_contexts(true) && self.visual_context == 0 && !self.rebuild_stacking_context
    }

    /// Whether the move changes only what the box paints and lays out, and the visual contexts it takes part in, and
    /// where `scroll_snaps` says a scroll container of the document may snap, nothing it snaps to.
    fn stays_in_its_box_and_visual_contexts(self, scroll_snaps: bool) -> bool {
        self.level < INVALIDATION_REBUILD_LAYOUT_TREE
            && !(scroll_snaps && self.resnap_scroll_container)
            && !self.repaint_text_decorations
    }

    fn unpack(packed: u32) -> Self {
        let has = |field: FfiStyleInvalidationField| packed & field as u32 != 0;
        Self {
            level: (packed & FfiStyleInvalidationField::LevelMask as u32) as u8,
            visual_context: ((packed >> FfiStyleInvalidationField::VisualContextShift as u32)
                & FfiStyleInvalidationField::LevelMask as u32) as u8,
            rebuild_root: ((packed >> FfiStyleInvalidationField::RebuildRootShift as u32)
                & FfiStyleInvalidationField::RebuildRootMask as u32) as u8,
            rebuild_stacking_context: has(FfiStyleInvalidationField::RebuildStackingContext),
            resnap_scroll_container: has(FfiStyleInvalidationField::ResnapScrollContainer),
            recompute_descendants: has(FfiStyleInvalidationField::RecomputeDescendants),
            inherited_groups: ((packed >> FfiStyleInvalidationField::InheritedGroupsShift as u32)
                & FfiStyleInvalidationField::InheritedGroupsMask as u32) as u8,
            repaint_text_decorations: has(FfiStyleInvalidationField::RepaintTextDecorations),
            non_inherited_inheritance_source: has(FfiStyleInvalidationField::NonInheritedInheritanceSource),
            any_computed_value_changed: has(FfiStyleInvalidationField::AnyComputedValueChanged),
            affects_hit_testing: has(FfiStyleInvalidationField::AffectsHitTesting),
            repaint_highlights: has(FfiStyleInvalidationField::RepaintHighlights),
        }
    }

    fn pack(self) -> u32 {
        let mut packed = u32::from(self.level) & FfiStyleInvalidationField::LevelMask as u32;
        packed |= u32::from(self.visual_context) << FfiStyleInvalidationField::VisualContextShift as u32;
        packed |= u32::from(self.rebuild_root) << FfiStyleInvalidationField::RebuildRootShift as u32;
        packed |= u32::from(self.rebuild_stacking_context) * FfiStyleInvalidationField::RebuildStackingContext as u32;
        packed |= u32::from(self.resnap_scroll_container) * FfiStyleInvalidationField::ResnapScrollContainer as u32;
        packed |= u32::from(self.recompute_descendants) * FfiStyleInvalidationField::RecomputeDescendants as u32;
        packed |= (u32::from(self.inherited_groups) & FfiStyleInvalidationField::InheritedGroupsMask as u32)
            << FfiStyleInvalidationField::InheritedGroupsShift as u32;
        packed |= u32::from(self.repaint_text_decorations) * FfiStyleInvalidationField::RepaintTextDecorations as u32;
        packed |= u32::from(self.non_inherited_inheritance_source)
            * FfiStyleInvalidationField::NonInheritedInheritanceSource as u32;
        packed |=
            u32::from(self.any_computed_value_changed) * FfiStyleInvalidationField::AnyComputedValueChanged as u32;
        packed |= u32::from(self.affects_hit_testing) * FfiStyleInvalidationField::AffectsHitTesting as u32;
        packed |= u32::from(self.repaint_highlights) * FfiStyleInvalidationField::RepaintHighlights as u32;
        packed
    }
}

/// What the damages of a row's moves, its element's and its pseudo-elements', tell the element's
/// children, in the shape the host reports an applied reaction with: the inherited style groups
/// that moved, and the facts the damage decides.
pub(super) fn child_reaction_facts_of_damage(damages: impl IntoIterator<Item = u32>) -> (u8, u32) {
    use super::bridge::style_reaction_applied_fact as fact;
    let mut damage = StyleInvalidation::default();
    for packed in damages {
        damage.merge(StyleInvalidation::unpack(packed));
    }
    let mut facts = 0;
    // As the host's invalidation reads it, a computed value that moved without a consequence asks
    // for nothing.
    if damage.level == 0
        && damage.visual_context == 0
        && !damage.rebuild_stacking_context
        && !damage.resnap_scroll_container
        && !damage.recompute_descendants
        && damage.inherited_groups == 0
        && !damage.repaint_highlights
        && !damage.affects_hit_testing
        && !damage.repaint_text_decorations
        && !damage.non_inherited_inheritance_source
    {
        facts |= fact::INVALIDATION_IS_NONE;
    }
    if damage.level >= INVALIDATION_REBUILD_LAYOUT_TREE {
        facts |= fact::NEEDS_LAYOUT_TREE_REBUILD;
    }
    if damage.recompute_descendants {
        facts |= fact::RECOMPUTE_DESCENDANT_STYLES;
    }
    (damage.inherited_groups, facts)
}

/// What a record move damages when the engine cannot read one of its records, or the host names no
/// style node for it: everything, which is always correct.
pub(super) fn unreadable_record_damage() -> u32 {
    debug_assert!(false, "record damage without a readable record");
    let mut damage = StyleInvalidation::full();
    damage.any_computed_value_changed = true;
    damage.pack()
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
        property_id::WILL_CHANGE => will_change_creates_stacking_context(values),
        _ => true,
    }
}

fn will_change_mentions(values: ComputedValuesView<'_>, predicate: impl Fn(u16) -> bool) -> bool {
    let implicit_will_change_values = values.misc_reset().implicit_will_change.as_slice();
    if implicit_will_change_values.iter().copied().any(&predicate) {
        return true;
    }
    let Some(StyleValueData::ValueList { values, .. }) = values.misc_reset().will_change.data() else {
        return false;
    };
    values.as_slice().iter().any(|entry| {
        let StyleValueData::CustomIdent { custom_ident } = entry.data() else {
            return false;
        };
        crate::css::serialize::with_fly_string_units(custom_ident, |units| {
            let property = match units {
                crate::css::serialize::StringUnits::Ascii(bytes) => property_metadata::property_id_from_name(bytes),
                crate::css::serialize::StringUnits::Utf16(units) => property_metadata::property_id_from_name(units),
            };
            property.is_some_and(&predicate)
        })
    })
}

fn will_change_creates_stacking_context(values: ComputedValuesView<'_>) -> bool {
    will_change_mentions(values, |property| {
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

// will-change names the property, or a member of the transform family for one of its members, so
// the element already has the stacking context, containing block and visual context node the
// property establishes at a non-initial value. A value change of that property leaves all three
// in place, and only the properties below are matched against will-change in style_queries.
fn will_change_covers_property(property: u16, values: ComputedValuesView<'_>) -> bool {
    match property {
        property_id::TRANSFORM | property_id::TRANSLATE | property_id::ROTATE | property_id::SCALE => {
            will_change_mentions(values, |named| {
                matches!(
                    named,
                    property_id::TRANSFORM | property_id::TRANSLATE | property_id::ROTATE | property_id::SCALE
                )
            })
        }
        property_id::OPACITY
        | property_id::FILTER
        | property_id::BACKDROP_FILTER
        | property_id::MIX_BLEND_MODE
        | property_id::PERSPECTIVE
        | property_id::TRANSFORM_STYLE
        | property_id::BACKFACE_VISIBILITY
        | property_id::CLIP_PATH
        | property_id::MASK_IMAGE
        | property_id::ISOLATION
        | property_id::CONTAIN
        | property_id::VIEW_TRANSITION_NAME => will_change_mentions(values, |named| named == property),
        _ => false,
    }
}

// The transform family, opacity and filter are the properties whose visual context node the
// visual context build creates ahead of the value when will-change names them.
fn will_change_promotes_visual_context_node(property: u16, values: ComputedValuesView<'_>) -> bool {
    matches!(
        property,
        property_id::TRANSFORM
            | property_id::TRANSLATE
            | property_id::ROTATE
            | property_id::SCALE
            | property_id::OPACITY
            | property_id::FILTER
    ) && will_change_covers_property(property, values)
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
        property_id::BACKGROUND_ATTACHMENT
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

fn accumulated_visual_context_change_alters_hit_test_items(
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
    false
}

// https://drafts.csswg.org/css-overflow-3/#overflow-propagation
// The document element and its first body child propagate their overflow to the viewport. Every full layout pass
// applies that propagation again from their computed values, and the change reaches the viewport through the
// relayout of the element's ancestors, so their layout boxes and subtrees can be kept.
fn viewport_propagated_overflow_invalidation() -> StyleInvalidation {
    let mut result = StyleInvalidation {
        rebuild_stacking_context: true,
        resnap_scroll_container: true,
        affects_hit_testing: true,
        ..StyleInvalidation::default()
    };
    result.ensure_level(INVALIDATION_RELAYOUT);
    result.ensure_visual_context(VISUAL_CONTEXT_REBUILD);
    result
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
    if matches!(
        property,
        property_id::COUNTER_RESET | property_id::COUNTER_SET | property_id::COUNTER_INCREMENT
    ) {
        let mut result = StyleInvalidation::default();
        result.ensure_level(INVALIDATION_REBUILD_LAYOUT_TREE);
        return result;
    }

    let mut result = StyleInvalidation {
        affects_hit_testing: property_metadata::property_affects_hit_testing(property),
        ..StyleInvalidation::default()
    };
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
    // A snap area is identified by its element when the visual context tree is built, so a box
    // that becomes or stops being one has its identity resolved again.
    if property == property_id::SCROLL_SNAP_ALIGN && old.has_scroll_snap_alignment() != new.has_scroll_snap_alignment()
    {
        result.ensure_visual_context(VISUAL_CONTEXT_UPDATE_VALUES);
    }
    let will_change_covers_property =
        will_change_covers_property(property, old) && will_change_covers_property(property, new);
    if property_metadata::property_affects_stacking_context(property)
        && (property == property_id::Z_INDEX
            || (!will_change_covers_property
                && value_creates_stacking_context(property, old) != value_creates_stacking_context(property, new)))
    {
        result.rebuild_stacking_context = true;
        result.ensure_level(INVALIDATION_REPAINT);
    }
    if new.transform().transform_style == crate::css::css_enums::transform_style::PRESERVE_3D
        && has_transform_style_grouping_property(old) != has_transform_style_grouping_property(new)
    {
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
                | property_id::BACKDROP_FILTER
                | property_id::MIX_BLEND_MODE
                | property_id::PERSPECTIVE
        ) && ((value_creates_stacking_context(property, old)
            && value_creates_stacking_context(property, new))
            || (will_change_promotes_visual_context_node(property, old)
                && will_change_promotes_visual_context_node(property, new))));
        result.ensure_visual_context(if value_only {
            VISUAL_CONTEXT_UPDATE_VALUES
        } else {
            VISUAL_CONTEXT_REBUILD
        });
        let alters_hit_test_items = accumulated_visual_context_change_alters_hit_test_items(property, old, new);
        result.affects_hit_testing |= alters_hit_test_items;
        if !alters_hit_test_items
            && !accumulated_visual_context_property_always_requires_repaint(property)
            && result.level < INVALIDATION_REPAINT
            && !result.recompute_descendants
        {
            needs_repaint = false;
        }
    }
    if needs_repaint || result.affects_hit_testing {
        result.ensure_level(INVALIDATION_REPAINT);
    }
    result
}

fn effective_value<'a>(view: &super::computed::StyleRecordView<'a>, property: u16) -> &'a StyleValueData {
    let index = usize::from(property - FIRST_LONGHAND_PROPERTY_ID);
    let table = unsafe { view.longhand_table.deref() };
    if let Some(entry) = unsafe { view.animated_overlay.as_ref() }.and_then(|overlay| overlay.get(property))
        && overlay_wins(entry, table.is_important(property))
    {
        return entry.value();
    }
    unsafe { view.longhand_values[index].cast::<StyleValueData>().deref() }
}

/// The value of `property` in the record whose table `table` is, with `overlay` over it in place of the record's own.
fn effective_value_with_overlay<'a>(
    table: &'a ComputedLonghandTable,
    overlay: Option<&'a AnimatedOverlay>,
    property: u16,
) -> Option<&'a StyleValueData> {
    if let Some(entry) = overlay.and_then(|overlay| overlay.get(property))
        && overlay_wins(entry, table.is_important(property))
    {
        return Some(entry.value());
    }
    table.get(property).map(|value| value.data())
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
    table: &ComputedLonghandTable,
    old_overlay: Option<&AnimatedOverlay>,
    new_overlay: Option<&AnimatedOverlay>,
    property: u16,
) -> bool {
    let old = effective_value_with_overlay(table, old_overlay, property);
    let new = effective_value_with_overlay(table, new_overlay, property);
    old.map(std::ptr::from_ref) != new.map(std::ptr::from_ref) && old != new
}

fn inheritance_dependent_value_changed(
    old: &ComputedLonghandTable,
    new: &ComputedLonghandTable,
    property: u16,
) -> bool {
    let value = |table: &ComputedLonghandTable| {
        table
            .inheritance_dependent_values()
            .find(|(candidate, _)| *candidate == property)
            .map(|(_, value)| value)
    };
    let old_value = value(old);
    let new_value = value(new);
    old_value != new_value
        && match (old_value, new_value) {
            (Some(old_value), Some(new_value)) => unsafe {
                *old_value.cast::<StyleValueData>() != *new_value.cast::<StyleValueData>()
            },
            _ => true,
        }
}

/// Whether replacing the record's `old_overlay` with `new_overlay` changes effective CSS values or the additional
/// properties to treat as included in will-change.
pub(crate) fn animation_overlay_changed(
    table: &ComputedLonghandTable,
    old_overlay: Option<&AnimatedOverlay>,
    new_overlay: Option<&AnimatedOverlay>,
) -> bool {
    if old_overlay.map_or(&[][..], |overlay| overlay.implicit_will_change.as_slice())
        != new_overlay.map_or(&[][..], |overlay| overlay.implicit_will_change.as_slice())
    {
        return true;
    }

    animation_overlay_properties(old_overlay, new_overlay)
        .any(|property| animation_value_changed(table, old_overlay, new_overlay, property))
}

impl RetainedState {
    pub(crate) fn compare_animation_overlay(
        &self,
        old_style_record: u64,
        animated_overlay: *const AnimatedOverlay,
        payloads: &[SharedPayload],
        is_document_element: bool,
    ) -> FfiAnimationInvalidation {
        let old_record = self
            .computed_group_sets
            .style_record_view(old_style_record)
            .unwrap_or_else(|| panic!("old style record {old_style_record:#x} is not live"));
        assert_eq!(payloads.len(), old_record.payloads.len());
        // SAFETY: A live record's table lives as long as the record.
        let table = unsafe { old_record.longhand_table.deref() };
        let old_overlay = unsafe { old_record.animated_overlay.as_ref() };
        let new_overlay = unsafe { animated_overlay.as_ref() };
        let old_values = ComputedValuesView::new(SharedPayload::as_pointer_slice(old_record.payloads));
        let new_values = ComputedValuesView::new(SharedPayload::as_pointer_slice(payloads));
        let mut ffi_result = FfiAnimationInvalidation::default();
        let mut invalidation = StyleInvalidation::default();
        let mut text_decoration_line_animated = false;

        if old_values.misc_reset().implicit_will_change != new_values.misc_reset().implicit_will_change {
            invalidation.merge(property_invalidation(property_id::WILL_CHANGE, old_values, new_values));
            ffi_result.changed_non_inherited_style_groups |=
                1 << crate::css::table_group_builder::group_index::MISC_RESET;
        }

        for property in animation_overlay_properties(old_overlay, new_overlay) {
            if !animation_value_changed(table, old_overlay, new_overlay, property) {
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

    /// Whether a sample of `node`'s animations, `animated_overlay` with the groups `payloads`, shown over
    /// `old_style_record`, the record the host installed for the element, changes only what the element's box paints
    /// and lays out: no layout tree, stacking context, visual context, scroll snap or text decoration of descendants.
    pub(crate) fn animation_sample_stays_in_its_box(
        &self,
        node: StyleNodeID,
        old_style_record: u64,
        animated_overlay: &AnimatedOverlay,
        payloads: &[SharedPayload],
        bounds: SampleBounds,
    ) -> bool {
        let is_document_element =
            self.computed_group_sets.adjustment_facts(node) & element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0;
        let invalidation = StyleInvalidation::unpack(
            self.compare_animation_overlay(old_style_record, animated_overlay, payloads, is_document_element)
                .invalidation,
        );
        let SampleBounds {
            scroll_snaps,
            subtree_follows,
        } = bounds;
        invalidation.stays_in_its_box_and_visual_contexts(scroll_snaps)
            || (subtree_follows
                && StyleInvalidation {
                    repaint_text_decorations: false,
                    ..invalidation
                }
                .stays_in_its_box_and_visual_contexts(scroll_snaps))
    }

    /// Whether moving `node` from `old_style_record`, the record the host installed for the element, to
    /// `new_style_record` changes only what the element's box paints and lays out, as a sample that stays in its box
    /// does, and nothing an element child inherits.
    pub(crate) fn restyle_stays_in_its_box(
        &mut self,
        node: StyleNodeID,
        old_style_record: u64,
        new_style_record: u64,
    ) -> bool {
        let invalidation =
            StyleInvalidation::unpack(self.element_record_damage(node, false, old_style_record, new_style_record));
        invalidation.stays_in_its_box()
            && !invalidation.recompute_descendants
            && ((invalidation.inherited_groups == 0 && !invalidation.non_inherited_inheritance_source)
                || self.tree.first_element_child(node).is_none())
    }

    /// What moving a box from the record `old_style_record` to `new_style_record` damages, of `properties` alone, read
    /// from the records' payloads: a sample of animations over a record keeps the record's longhand table, and holds
    /// the values it animates only in its payloads.
    pub(crate) fn sampled_properties_damage(
        &self,
        old_style_record: u64,
        new_style_record: u64,
        properties: &[u16],
    ) -> u32 {
        if properties.is_empty() || old_style_record == new_style_record {
            return 0;
        }
        let (Some(old_record), Some(new_record)) = (
            self.computed_group_sets.style_record_view(old_style_record),
            self.computed_group_sets.style_record_view(new_style_record),
        ) else {
            return unreadable_record_damage();
        };
        let old_values = ComputedValuesView::new(SharedPayload::as_pointer_slice(old_record.payloads));
        let new_values = ComputedValuesView::new(SharedPayload::as_pointer_slice(new_record.payloads));
        let mut result = StyleInvalidation::default();
        for &property in properties {
            result.merge(property_invalidation(property, old_values, new_values));
        }
        result.pack()
    }

    pub(crate) fn compare_style_records(
        &mut self,
        old_style_record: u64,
        new_style_record: u64,
        font_lists_equal: bool,
        element_folds_transform_into_layout: bool,
        element_propagates_overflow_to_viewport: bool,
    ) -> u32 {
        let key = (
            old_style_record,
            new_style_record,
            font_lists_equal,
            element_folds_transform_into_layout,
            element_propagates_overflow_to_viewport,
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
        let old_values = ComputedValuesView::new(SharedPayload::as_pointer_slice(old_record.payloads));
        let new_values = ComputedValuesView::new(SharedPayload::as_pointer_slice(new_record.payloads));
        let old_table = unsafe { old_record.longhand_table.deref() };
        let new_table = unsafe { new_record.longhand_table.deref() };
        let all_groups_equal = old_record
            .payloads
            .iter()
            .zip(new_record.payloads)
            .enumerate()
            .all(|(index, (&old, &new))| old == new || style_group_payloads_equal(index, old.as_ptr(), new.as_ptr()));
        let can_skip = all_groups_equal
            && std::ptr::eq(old_table, new_table)
            && font_lists_equal
            && old_record.animated_overlay.is_null()
            && new_record.animated_overlay.is_null();
        let mut result = StyleInvalidation::default();
        if !font_lists_equal
            || ((old_values.continue_() != crate::css::css_enums::continue_value::AUTO
                || new_values.continue_() != crate::css::css_enums::continue_value::AUTO)
                && old_values.display_before_box_type_transformation()
                    != new_values.display_before_box_type_transformation())
        {
            result.any_computed_value_changed = true;
            result.ensure_level(INVALIDATION_RELAYOUT);
        }
        if !can_skip {
            if old_values.misc_reset().implicit_will_change != new_values.misc_reset().implicit_will_change {
                result.merge(property_invalidation(property_id::WILL_CHANGE, old_values, new_values));
            }
            // Equal resolved values can still inherit differently: currentcolor and an RGB color
            // may paint the same here, but resolve to different colors in an inheriting child.
            for (property, _) in old_table
                .inheritance_dependent_values()
                .chain(new_table.inheritance_dependent_values())
            {
                if !inheritance_dependent_value_changed(old_table, new_table, property) {
                    continue;
                }
                result.any_computed_value_changed = true;
                if property_metadata::property_is_inherited(property) {
                    match property_metadata::property_style_group_index(property) {
                        Some(group) if group < 7 => result.inherited_groups |= 1 << group,
                        _ => result.inherited_groups = ALL_INHERITED_STYLE_GROUPS,
                    }
                } else {
                    result.non_inherited_inheritance_source = true;
                }
            }
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
                let mut invalidation = if element_propagates_overflow_to_viewport
                    && matches!(property, property_id::OVERFLOW_X | property_id::OVERFLOW_Y)
                {
                    viewport_propagated_overflow_invalidation()
                } else {
                    property_invalidation(property, old_values, new_values)
                };
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
                        old_record.longhand_values[usize::from(old_physical - FIRST_LONGHAND_PROPERTY_ID)]
                            .cast::<StyleValueData>()
                            .deref()
                    };
                    let new_base = unsafe {
                        new_record.longhand_values[usize::from(new_physical - FIRST_LONGHAND_PROPERTY_ID)]
                            .cast::<StyleValueData>()
                            .deref()
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

    /// What moving `node`, or one of its pseudo-elements, from one record to another damages, read
    /// from the two records and the facts the engine holds of the element and its place in the
    /// tree. What the element's box was built with is left to the host: the counter styles it
    /// resolved.
    pub(crate) fn element_record_damage(
        &mut self,
        node: StyleNodeID,
        is_pseudo_element: bool,
        old_style_record: u64,
        new_style_record: u64,
    ) -> u32 {
        let (font_lists_equal, color_changed, stroke_uses_current_color, table_fixup_child_changed, overflow_changed) = {
            let (Some(old_record), Some(new_record)) = (
                self.computed_group_sets.style_record_view(old_style_record),
                self.computed_group_sets.style_record_view(new_style_record),
            ) else {
                return unreadable_record_damage();
            };
            let old_values = ComputedValuesView::new(SharedPayload::as_pointer_slice(old_record.payloads));
            let new_values = ComputedValuesView::new(SharedPayload::as_pointer_slice(new_record.payloads));
            let stroke_uses_current_color = |values: ComputedValuesView<'_>| {
                let stroke = &values.inherited_svg().stroke;
                stroke.kind != SVG_PAINT_NONE && stroke.color_is_currentcolor
            };
            // The table fixup algorithm needs an authored box's display from before box type
            // transformation. A flex or grid item can therefore keep the same blockified display
            // while changing whether it needs anonymous table wrappers.
            let is_table_fixup_child = |values: ComputedValuesView<'_>| {
                let display = values.display_before_box_type_transformation();
                display.is_table_row_group()
                    || display.is_table_header_group()
                    || display.is_table_footer_group()
                    || display.is_table_column_group()
                    || display.is_table_caption()
            };
            (
                old_values
                    .font()
                    .font_cascade_list
                    .resolves_like(&new_values.font().font_cascade_list),
                old_values.inherited_text().color != new_values.inherited_text().color,
                stroke_uses_current_color(old_values) || stroke_uses_current_color(new_values),
                is_table_fixup_child(old_values) != is_table_fixup_child(new_values),
                old_values.box_values().overflow_x != new_values.box_values().overflow_x
                    || old_values.box_values().overflow_y != new_values.box_values().overflow_y,
            )
        };
        let is_svg_graphics_element =
            self.computed_group_sets.adjustment_facts(node) & element_adjustment_fact::IS_SVG_GRAPHICS_ELEMENT != 0;
        let packed = self.compare_style_records(
            old_style_record,
            new_style_record,
            font_lists_equal,
            is_svg_graphics_element && self.element_folds_transform_into_svg_container_layout(node),
            !is_pseudo_element && self.element_propagates_overflow_to_viewport(node),
        );
        let mut damage = StyleInvalidation::unpack(packed);
        // Fieldsets with a rendered legend or flex contents transfer overflow to an anonymous
        // content box during tree construction. Rebuild to refresh both boxes' overrides.
        if !is_pseudo_element && overflow_changed && self.element_box_kind(node) == ElementBoxKind::FieldSet {
            damage.merge(StyleInvalidation::rebuild_layout_tree_from(REBUILD_ROOT_SELF));
        }
        // An SVG currentColor stroke stores its resolved color alongside the fact that it came from
        // currentColor. A color-only change can therefore alter the visible stroke width and the SVG
        // container bounds without changing the stroke longhand itself.
        if is_svg_graphics_element && color_changed && stroke_uses_current_color {
            damage.ensure_level(INVALIDATION_RELAYOUT);
        }
        // Generated pseudo-element boxes are anonymous, so table fixup uses their adjusted display
        // instead.
        if !is_pseudo_element && table_fixup_child_changed {
            damage.any_computed_value_changed = true;
            damage.merge(StyleInvalidation::full());
        }
        damage.pack() | (packed & FfiStyleInvalidationField::CacheHit as u32)
    }

    /// What moving `node`'s pseudo-element of `pseudo_kind` from one record to another damages.
    /// Either record can be absent (zero): the pseudo-element's box then appears or goes away. The
    /// originating element holds `originating_style_record`, which decides whether the box stays
    /// inside the originating one. `counter_styles_changed` is the host's answer for what the box
    /// was built with: the counter styles its generated content and marker resolved.
    pub(crate) fn pseudo_element_record_damage(
        &mut self,
        node: StyleNodeID,
        pseudo_kind: u8,
        old_style_record: u64,
        new_style_record: u64,
        originating_style_record: u64,
        counter_styles_changed: bool,
    ) -> u32 {
        use super::publication::pseudo_kind::{AFTER, BEFORE, MARKER};
        if old_style_record == new_style_record {
            return 0;
        }
        // NB: Highlight pseudo-elements do not generate boxes or affect layout.
        if super::publication::pseudo_kind::is_highlight(pseudo_kind) {
            return StyleInvalidation {
                level: INVALIDATION_REPAINT,
                repaint_highlights: true,
                any_computed_value_changed: true,
                ..StyleInvalidation::default()
            }
            .pack();
        }
        // A zero record is an absent one; a nonzero record the engine cannot read is none at all.
        let view = |record: u64| match record {
            0 => Some(None),
            record => self
                .computed_group_sets
                .style_record_view(record)
                .map(|view| Some(ComputedValuesView::new(SharedPayload::as_pointer_slice(view.payloads)))),
        };
        let (Some(old_values), Some(new_values), Some(Some(originating_values))) = (
            view(old_style_record),
            view(new_style_record),
            view(originating_style_record),
        ) else {
            return unreadable_record_damage();
        };

        // A non-inline generated box can split an inline originating element and mutate anonymous
        // structure in its parent. Inline ::before and ::after boxes remain confined to the
        // originating element's layout subtree.
        let originating_is_inline_outside = originating_values.display().is_inline_outside();
        let can_escape_originating_element = |values: Option<ComputedValuesView<'_>>| {
            values.is_some_and(|values| {
                let display = values.display();
                originating_is_inline_outside
                    && !display.is_none()
                    && !display.is_contents()
                    && !display.is_inline_outside()
            })
        };
        // A marker box is always attached inside the originating box, as is a ::before or ::after
        // box that cannot escape it, so replacing that box in place creates, removes, or rebuilds
        // the pseudo-element box along with it.
        let box_stays_inside_originating_box = pseudo_kind == MARKER
            || ((pseudo_kind == BEFORE || pseudo_kind == AFTER)
                && !can_escape_originating_element(old_values)
                && !can_escape_originating_element(new_values));
        let has_independent_content = |values: Option<ComputedValuesView<'_>>| {
            let Some(values) = values else {
                return true;
            };
            let display = values.display();
            !display.is_list_item()
                && !display.is_contents()
                && values.counter_reset().is_empty()
                && values.counter_increment().is_empty()
                && values.counter_set().is_empty()
                && (values.content_is_keyword() || values.content_is_strings_only())
        };
        let can_update_in_place = (pseudo_kind == BEFORE || pseudo_kind == AFTER)
            && has_independent_content(old_values)
            && has_independent_content(new_values);

        let (Some(old_values), Some(new_values)) = (old_values, new_values) else {
            // The box appears or goes away.
            let rebuild_root = match (box_stays_inside_originating_box, can_update_in_place) {
                (false, _) => REBUILD_ROOT_PARENT,
                (true, false) => REBUILD_ROOT_SELF,
                (true, true) => REBUILD_ROOT_PSEUDO_ELEMENTS,
            };
            let mut damage = StyleInvalidation::rebuild_layout_tree_from(rebuild_root);
            damage.any_computed_value_changed = true;
            return damage.pack();
        };
        let old_display = old_values.display();
        let new_display = new_values.display();

        let packed = self.element_record_damage(node, true, old_style_record, new_style_record);
        let mut damage = StyleInvalidation::unpack(packed);
        // Generated content and the marker live inside the box's own layout subtree, so, like a
        // 'content' change, a counter style change rebuilds from the box rather than its parent.
        if counter_styles_changed {
            damage.merge(StyleInvalidation::rebuild_layout_tree_from(REBUILD_ROOT_SELF));
        }
        // A display: contents pseudo-element has no principal layout node to receive its updated
        // style. A list-item pseudo-element also owns a generated marker whose layout state is not
        // updated through the originating element. Rebuild their layout subtrees when a style change
        // otherwise requires relayout.
        if damage.level == INVALIDATION_RELAYOUT
            && (old_display.is_contents()
                || old_display.is_list_item()
                || new_display.is_contents()
                || new_display.is_list_item())
        {
            damage.ensure_level(INVALIDATION_REBUILD_LAYOUT_TREE);
        }
        if damage.level >= INVALIDATION_REBUILD_LAYOUT_TREE && damage.rebuild_root != REBUILD_ROOT_PARENT {
            if !box_stays_inside_originating_box {
                damage.ensure_level(INVALIDATION_REBUILD_LAYOUT_TREE);
            } else if damage.rebuild_root == REBUILD_ROOT_BOX_PRESENCE_CHANGE {
                damage.rebuild_root = REBUILD_ROOT_SELF;
            }
        }
        if damage.level >= INVALIDATION_REBUILD_LAYOUT_TREE
            && damage.rebuild_root == REBUILD_ROOT_SELF
            && can_update_in_place
        {
            damage.rebuild_root = REBUILD_ROOT_PSEUDO_ELEMENTS;
        }
        damage.pack() | (packed & FfiStyleInvalidationField::CacheHit as u32)
    }

    // SVG container layout unions each child's bounding box mapped by the child's own transform. An
    // outermost <svg> is laid out by its CSS parent (as is one re-rooted by foreignObject), so its
    // own transform stays paint-only like any CSS box.
    fn element_folds_transform_into_svg_container_layout(&self, node: StyleNodeID) -> bool {
        let Some(parent) = self.tree.parent(node) else {
            return false;
        };
        let parent_facts = self.computed_group_sets.adjustment_facts(parent);
        parent_facts & element_adjustment_fact::IS_SVG_ELEMENT != 0
            && parent_facts & element_adjustment_fact::IS_SVG_FOREIGN_OBJECT_ELEMENT == 0
    }

    // https://drafts.csswg.org/css-overflow-3/#overflow-propagation
    // The root element and, for an HTML <html> root, its first <body> child are the elements whose
    // overflow every full layout pass reads for viewport propagation.
    fn element_propagates_overflow_to_viewport(&self, node: StyleNodeID) -> bool {
        let facts = |node: StyleNodeID| self.computed_group_sets.adjustment_facts(node);
        if facts(node) & element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0 {
            return true;
        }
        if facts(node) & element_adjustment_fact::IS_HTML_BODY_ELEMENT == 0 {
            return false;
        }
        let Some(parent) = self.tree.parent(node) else {
            return false;
        };
        let root_facts = element_adjustment_fact::IS_DOCUMENT_ELEMENT | element_adjustment_fact::IS_HTML_HTML_ELEMENT;
        if facts(parent) & root_facts != root_facts {
            return false;
        }
        let mut child = self.tree.first_element_child(parent);
        while let Some(candidate) = child {
            if facts(candidate) & element_adjustment_fact::IS_HTML_BODY_ELEMENT != 0 {
                return candidate == node;
            }
            child = self.tree.next_element_sibling(candidate);
        }
        false
    }
}

const _: () = assert!(FIRST_LONGHAND_PROPERTY_ID <= LAST_LONGHAND_PROPERTY_ID);
