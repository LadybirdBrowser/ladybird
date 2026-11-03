/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::bridge::{ElementBoxKind, FfiAnimationInvalidation, FfiStyleInvalidationField, element_adjustment_fact};
use super::computed::{ComputedGroupSets, FinalStyleRecordID, StyleRecordView, ViewLonghands};
use super::{RetainedState, StyleNodeID};
use crate::css::animated_overlay::{AnimatedOverlay, overlay_wins};
use crate::css::computed_value_types::SVG_PAINT_NONE;
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::property_metadata::{
    self, FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, NUMBER_OF_LONGHAND_PROPERTIES, property_id,
};
use crate::css::style_value::TransformationPayload;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};

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

/// A style record read through its view, with the identity it is live under where it is installed,
/// which memoizes comparisons between installed records. A record not installed yet has none.
#[derive(Clone, Copy)]
pub(crate) struct RecordView<'a> {
    pub values: StyleRecordView<'a>,
    pub identity: Option<FinalStyleRecordID>,
}

impl<'a> RecordView<'a> {
    /// The view of the live record `raw_style_record` names, if the engine can read it.
    pub(crate) fn installed(sets: &'a ComputedGroupSets, raw_style_record: u64) -> Option<Self> {
        Some(Self {
            values: sets.style_record_view(raw_style_record)?,
            identity: FinalStyleRecordID::from_raw(raw_style_record),
        })
    }

    fn computed_values(&self) -> ComputedValuesView<'a> {
        ComputedValuesView::of(self.values.payloads)
    }
}

/// An element, or one of its pseudo-elements, moving from one record to another.
#[derive(Clone, Copy)]
pub(crate) struct RecordMove<'a> {
    pub old: RecordView<'a>,
    pub new: RecordView<'a>,
}

/// A pseudo-element moving from one record to another, where either can be absent: its box then
/// appears or goes away.
#[derive(Clone, Copy)]
pub(crate) struct PseudoElementRecordMove<'a> {
    pub old: Option<RecordView<'a>>,
    pub new: Option<RecordView<'a>>,
}

/// The facts the engine holds of an element and its place in the tree that what moving it, or one
/// of its pseudo-elements, between records damages depends on.
#[derive(Clone, Copy)]
pub(crate) struct ElementDamageFacts {
    is_pseudo_element: bool,
    is_svg_graphics_element: bool,
    folds_transform_into_layout: bool,
    propagates_overflow_to_viewport: bool,
    is_field_set: bool,
    verifies_reads: bool,
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
    white_space_collapse_changed: bool,
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
        self.white_space_collapse_changed |= other.white_space_collapse_changed;
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
            white_space_collapse_changed: has(FfiStyleInvalidationField::WhiteSpaceCollapseChanged),
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
        packed |=
            u32::from(self.white_space_collapse_changed) * FfiStyleInvalidationField::WhiteSpaceCollapseChanged as u32;
        packed
    }
}

/// What the damages of an element's moves, its element's and its pseudo-elements', have the main
/// thread do beyond the element's box, and where a layout tree rebuild they ask for starts. An
/// element with no damage to its own move, as on its first style, rebuilds from its parent.
pub(super) fn main_effects_of_damage(
    element: Option<u32>,
    pseudo_elements: impl IntoIterator<Item = u32>,
) -> (super::bridge::MainEffects, super::bridge::LayoutTreeRebuildRoot) {
    use super::bridge::{LayoutTreeRebuildRoot, MainEffects};
    let mut damage = element.map_or_else(StyleInvalidation::full, StyleInvalidation::unpack);
    for packed in pseudo_elements {
        damage.merge(StyleInvalidation::unpack(packed));
    }
    let rebuilds = damage.level >= INVALIDATION_REBUILD_LAYOUT_TREE;
    let effects = MainEffects::NONE
        .with(MainEffects::REBUILD, rebuilds)
        .with(MainEffects::SCROLL_SNAP, damage.resnap_scroll_container)
        .with(MainEffects::HIGHLIGHTS, damage.repaint_highlights && !rebuilds)
        .with(MainEffects::HOST_BOX_FACTS, damage.white_space_collapse_changed);
    (effects, LayoutTreeRebuildRoot::from_raw(damage.rebuild_root))
}

/// The damages of an element's moves, its element's and its pseudo-elements', as one word. An
/// element with no damage to its own move, as on its first style, is damaged entirely.
pub(super) fn merged_damage(element: Option<u32>, pseudo_elements: impl IntoIterator<Item = u32>) -> u32 {
    let mut damage = element.map_or_else(StyleInvalidation::full, StyleInvalidation::unpack);
    for packed in pseudo_elements {
        damage.merge(StyleInvalidation::unpack(packed));
    }
    damage.pack()
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
        property_id::VIEW_TRANSITION_NAME => {
            !style_value_is_none(values.misc_interaction().view_transition_name.data())
        }
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
    let implicit_will_change_values = values.misc_interaction().implicit_will_change.as_slice();
    if implicit_will_change_values.iter().copied().any(&predicate) {
        return true;
    }
    let Some(StyleValueData::ValueList { values, .. }) = values.misc_interaction().will_change.data() else {
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
    let effects = values.effects();
    let mask = values.mask();
    let overflow_groups = |overflow| {
        overflow != crate::css::css_enums::overflow::VISIBLE && overflow != crate::css::css_enums::overflow::CLIP
    };
    overflow_groups(values.overflow_x())
        || overflow_groups(values.overflow_y())
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
        let StyleValueData::Transformation(transformation_payload) = value else {
            return None;
        };
        let TransformationPayload {
            transform_function,
            values,
            matrix_values,
            ..
        } = &**transformation_payload;
        let matrix = crate::css::table_group_builder::transformation_to_matrix(
            *transform_function,
            if matrix_values.as_slice().is_empty() {
                values.as_slice()
            } else {
                matrix_values.as_slice()
            },
        );
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
    // NB: A basic shape, a geometry box, or both clip with a path, which a visual context frame holds.
    match values.mask().clip_path.data() {
        Some(StyleValueData::BasicShape { .. } | StyleValueData::ValueList { .. }) => true,
        Some(StyleValueData::Keyword { keyword }) => *keyword != crate::css::css_enums::keyword::NONE,
        _ => false,
    }
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
    // An element whose content is a single image generates another box than one that does not, which its parent's fixup
    // and wrappers have to take in.
    if property == property_id::CONTENT && old.content_is_single_image() != new.content_is_single_image() {
        return StyleInvalidation::full();
    }
    if matches!(
        property,
        property_id::APPEARANCE | property_id::CONTENT | property_id::CONTENT_VISIBILITY | property_id::TEXT_TRANSFORM
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
    // The clip that `overflow: clip` gives a box's visual context reaches out by the box's overflow-clip-margin.
    if matches!(
        property,
        property_id::OVERFLOW_CLIP_MARGIN_TOP
            | property_id::OVERFLOW_CLIP_MARGIN_RIGHT
            | property_id::OVERFLOW_CLIP_MARGIN_BOTTOM
            | property_id::OVERFLOW_CLIP_MARGIN_LEFT
    ) {
        result.ensure_visual_context(VISUAL_CONTEXT_UPDATE_VALUES);
    }
    if property == property_id::WHITE_SPACE_COLLAPSE {
        let collapse = crate::css::css_enums::white_space_collapse::COLLAPSE;
        result.white_space_collapse_changed =
            (old.white_space_collapse() == collapse) != (new.white_space_collapse() == collapse);
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
        result.repaint_text_decorations =
            property != property_id::COLOR || old.text_decoration_color() != new.text_decoration_color();
    }
    // https://drafts.csswg.org/css-viewport/#effective-zoom
    // NB: The lengths of each descendant are zoomed by its effective zoom, which the element's zoom is part of.
    if matches!(
        property,
        property_id::DIRECTION | property_id::WRITING_MODE | property_id::ZOOM
    ) {
        result.recompute_descendants = true;
    }
    if property == property_id::VISIBILITY {
        let collapse = crate::css::css_enums::visibility::COLLAPSE;
        if (old.visibility() == collapse) != (new.visibility() == collapse) {
            result.ensure_level(INVALIDATION_RELAYOUT);
        }
        // Only a visible box has its backdrop filtered.
        let visible = crate::css::css_enums::visibility::VISIBLE;
        if (old.visibility() == visible) != (new.visibility() == visible)
            && (old.effects().backdrop_filter.operations.length != 0
                || new.effects().backdrop_filter.operations.length != 0)
        {
            result.ensure_visual_context(VISUAL_CONTEXT_UPDATE_VALUES);
        }
        result.ensure_level(INVALIDATION_REPAINT);
    } else if property_metadata::property_affects_layout(property) {
        result.ensure_level(INVALIDATION_RELAYOUT);
    }
    if property_metadata::property_affects_scrollable_overflow(property)
        || property_metadata::property_may_resnap_scroll_container(property)
    {
        result.resnap_scroll_container = true;
    }
    // A snap area is identified by its element when the visual context tree is built, so a box
    // that becomes or stops being one has its identity resolved again.
    if property == property_id::SCROLL_SNAP_ALIGN && old.has_scroll_snap_alignment() != new.has_scroll_snap_alignment()
    {
        result.ensure_visual_context(VISUAL_CONTEXT_UPDATE_VALUES);
    }
    // A scroll container that keeps its scrolls has a scroll node of its own, so a box that starts or stops keeping
    // them has its visual context nodes built again.
    if matches!(
        property,
        property_id::OVERSCROLL_BEHAVIOR_X | property_id::OVERSCROLL_BEHAVIOR_Y
    ) {
        use crate::css::css_enums::overscroll_behavior::{AUTO, CHAIN};
        let chains = |value: u8| value == AUTO || value == CHAIN;
        let old_values = old.misc_interaction();
        let new_values = new.misc_interaction();
        if chains(old_values.overscroll_behavior_x) != chains(new_values.overscroll_behavior_x)
            || chains(old_values.overscroll_behavior_y) != chains(new_values.overscroll_behavior_y)
        {
            result.ensure_visual_context(VISUAL_CONTEXT_REBUILD);
        }
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

/// The value of `property` in the record viewed by `view`, whose longhands are `longhands`, with
/// the view's animated values over them.
fn effective_value<'a>(
    view: &super::computed::StyleRecordView<'a>,
    longhands: ViewLonghands<'a>,
    property: u16,
) -> Option<&'a StyleValueData> {
    if let Some(entry) = view.animated_overlay.and_then(|overlay| overlay.get(property))
        && overlay_wins(entry, longhands.is_important(property))
    {
        return Some(entry.value());
    }
    longhands.value(property)
}

fn same_value(first: Option<&StyleValueData>, second: Option<&StyleValueData>) -> bool {
    match (first, second) {
        (Some(first), Some(second)) => std::ptr::eq(first, second) || first == second,
        (None, None) => true,
        _ => false,
    }
}

/// The value of `property` in the record whose longhands `longhands` are, with `overlay` over it in place of the
/// record's own.
fn effective_value_with_overlay<'a>(
    longhands: ViewLonghands<'a>,
    overlay: Option<&'a AnimatedOverlay>,
    property: u16,
) -> Option<&'a StyleValueData> {
    longhands
        .overlay_value(overlay, property)
        .or_else(|| longhands.handle(property))
        .map(RetainedStyleValueData::data)
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
    longhands: ViewLonghands<'_>,
    old_overlay: Option<&AnimatedOverlay>,
    new_overlay: Option<&AnimatedOverlay>,
    property: u16,
) -> bool {
    let old = effective_value_with_overlay(longhands, old_overlay, property);
    let new = effective_value_with_overlay(longhands, new_overlay, property);
    old.map(std::ptr::from_ref) != new.map(std::ptr::from_ref) && old != new
}

/// Whether replacing the record's `old_overlay` with `new_overlay` changes effective CSS values or the additional
/// properties to treat as included in will-change.
pub(crate) fn animation_overlay_changed(
    longhands: ViewLonghands<'_>,
    old_overlay: Option<&AnimatedOverlay>,
    new_overlay: Option<&AnimatedOverlay>,
) -> bool {
    if old_overlay.map_or(&[][..], |overlay| overlay.implicit_will_change.as_slice())
        != new_overlay.map_or(&[][..], |overlay| overlay.implicit_will_change.as_slice())
    {
        return true;
    }

    animation_overlay_properties(old_overlay, new_overlay)
        .any(|property| animation_value_changed(longhands, old_overlay, new_overlay, property))
}

impl RetainedState {
    pub(crate) fn compare_animation_overlay(
        &self,
        old_record: StyleRecordView<'_>,
        animated_overlay: *const AnimatedOverlay,
        payloads: &crate::css::computed_values::StyleGroups,
        is_document_element: bool,
    ) -> FfiAnimationInvalidation {
        let longhands = old_record.longhands();
        let old_overlay = old_record.animated_overlay;
        let new_overlay = unsafe { animated_overlay.as_ref() };
        let old_values = ComputedValuesView::of(old_record.payloads);
        let new_values = ComputedValuesView::of(payloads);
        let mut ffi_result = FfiAnimationInvalidation::default();
        let mut invalidation = StyleInvalidation::default();
        let mut text_decoration_line_animated = false;

        if old_values.misc_interaction().implicit_will_change != new_values.misc_interaction().implicit_will_change {
            invalidation.merge(property_invalidation(property_id::WILL_CHANGE, old_values, new_values));
        }

        for property in animation_overlay_properties(old_overlay, new_overlay) {
            if !animation_value_changed(longhands, old_overlay, new_overlay, property) {
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
        old_record: StyleRecordView<'_>,
        animated_overlay: &AnimatedOverlay,
        payloads: &crate::css::computed_values::StyleGroups,
        bounds: SampleBounds,
    ) -> bool {
        let is_document_element =
            self.projection.adjustment_facts(node) & element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0;
        let invalidation = StyleInvalidation::unpack(
            self.compare_animation_overlay(old_record, animated_overlay, payloads, is_document_element)
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
                || self.projection.first_element_child(node).is_none())
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
        let old_values = ComputedValuesView::of(old_record.payloads);
        let new_values = ComputedValuesView::of(new_record.payloads);
        let mut result = StyleInvalidation::default();
        for &property in properties {
            result.merge(property_invalidation(property, old_values, new_values));
        }
        result.pack()
    }

    /// The facts of `node` that what moving it, or one of its pseudo-elements, between records
    /// damages depends on.
    pub(crate) fn element_damage_facts(&self, node: StyleNodeID, is_pseudo_element: bool) -> ElementDamageFacts {
        let is_svg_graphics_element =
            self.projection.adjustment_facts(node) & element_adjustment_fact::IS_SVG_GRAPHICS_ELEMENT != 0;
        ElementDamageFacts {
            is_pseudo_element,
            is_svg_graphics_element,
            folds_transform_into_layout: is_svg_graphics_element
                && self.element_folds_transform_into_svg_container_layout(node),
            propagates_overflow_to_viewport: !is_pseudo_element && self.element_propagates_overflow_to_viewport(node),
            is_field_set: !is_pseudo_element && self.element_box_kind(node) == ElementBoxKind::FieldSet,
            verifies_reads: self.process_style.verifies_reads,
        }
    }

    /// What moving `node`, or one of its pseudo-elements, from one installed record to another
    /// damages, read from the two records and the facts the engine holds of the element and its
    /// place in the tree.
    pub(crate) fn element_record_damage(
        &mut self,
        node: StyleNodeID,
        is_pseudo_element: bool,
        old_style_record: u64,
        new_style_record: u64,
    ) -> u32 {
        let facts = self.element_damage_facts(node, is_pseudo_element);
        let sets = &self.computed_group_sets;
        let (Some(old), Some(new)) = (
            RecordView::installed(sets, old_style_record),
            RecordView::installed(sets, new_style_record),
        ) else {
            return unreadable_record_damage();
        };
        element_record_damage_of_views(facts, RecordMove { old, new })
    }

    /// What moving `node`'s pseudo-element of `pseudo_kind` from one installed record to another
    /// damages. Either record can be absent (zero): the pseudo-element's box then appears or goes
    /// away. The originating element holds `originating_style_record`.
    pub(crate) fn pseudo_element_record_damage(
        &mut self,
        node: StyleNodeID,
        pseudo_kind: u8,
        old_style_record: u64,
        new_style_record: u64,
        originating_style_record: u64,
        counter_styles_changed: bool,
    ) -> u32 {
        if let Some(damage) = pseudo_element_damage_without_records(pseudo_kind, old_style_record == new_style_record) {
            return damage;
        }
        let facts = self.element_damage_facts(node, true);
        let sets = &self.computed_group_sets;
        // A zero record is an absent one; a nonzero record the engine cannot read is none at all.
        let view = |record: u64| match record {
            0 => Some(None),
            record => RecordView::installed(sets, record).map(Some),
        };
        let (Some(old), Some(new), Some(Some(originating))) = (
            view(old_style_record),
            view(new_style_record),
            view(originating_style_record),
        ) else {
            return unreadable_record_damage();
        };
        pseudo_element_record_damage_of_views(
            facts,
            pseudo_kind,
            PseudoElementRecordMove { old, new },
            originating,
            counter_styles_changed,
        )
    }

    // SVG container layout unions each child's bounding box mapped by the child's own transform. An
    // outermost <svg> is laid out by its CSS parent (as is one re-rooted by foreignObject), so its
    // own transform stays paint-only like any CSS box.
    fn element_folds_transform_into_svg_container_layout(&self, node: StyleNodeID) -> bool {
        let Some(parent) = self.projection.parent(node) else {
            return false;
        };
        let parent_facts = self.projection.adjustment_facts(parent);
        parent_facts & element_adjustment_fact::IS_SVG_ELEMENT != 0
            && parent_facts & element_adjustment_fact::IS_SVG_FOREIGN_OBJECT_ELEMENT == 0
    }

    // https://drafts.csswg.org/css-overflow-3/#overflow-propagation
    // The root element and, for an HTML <html> root, its first <body> child are the elements whose
    // overflow every full layout pass reads for viewport propagation.
    fn element_propagates_overflow_to_viewport(&self, node: StyleNodeID) -> bool {
        let facts = |node: StyleNodeID| self.projection.adjustment_facts(node);
        if facts(node) & element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0 {
            return true;
        }
        if facts(node) & element_adjustment_fact::IS_HTML_BODY_ELEMENT == 0 {
            return false;
        }
        let Some(parent) = self.projection.parent(node) else {
            return false;
        };
        let root_facts = element_adjustment_fact::IS_DOCUMENT_ELEMENT | element_adjustment_fact::IS_HTML_HTML_ELEMENT;
        if facts(parent) & root_facts != root_facts {
            return false;
        }
        let mut child = self.projection.first_element_child(parent);
        while let Some(candidate) = child {
            if facts(candidate) & element_adjustment_fact::IS_HTML_BODY_ELEMENT != 0 {
                return candidate == node;
            }
            child = self.projection.next_element_sibling(candidate);
        }
        false
    }
}

/// What moving between the records of `record_move` damages, for an element with the given facts.
fn compare_record_views(
    record_move: RecordMove<'_>,
    font_lists_equal: bool,
    element_folds_transform_into_layout: bool,
    element_propagates_overflow_to_viewport: bool,
    verifies_reads: bool,
) -> u32 {
    let (old_longhands, new_longhands) = (record_move.old.values.longhands(), record_move.new.values.longhands());
    let packed = record_move_damage(
        record_move,
        [old_longhands, new_longhands],
        font_lists_equal,
        element_folds_transform_into_layout,
        element_propagates_overflow_to_viewport,
    )
    .pack();
    // The damage takes the longhands of the groups both records share as unmoved: each of them
    // must read the very same value from both records.
    if verifies_reads
        && let Some(shared) = old_longhands.shared_with(new_longhands)
        && (crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
            ..=crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID)
            .any(|property| {
                shared.holds(property)
                    && old_longhands.handle(property).map(RetainedStyleValueData::pointer)
                        != new_longhands.handle(property).map(RetainedStyleValueData::pointer)
            })
    {
        eprintln!("the engine records that share a group read different values of a longhand it holds");
        std::process::abort();
    }
    packed
}

/// What moving between the records of `record_move`, whose longhands are `longhands`, damages.
fn record_move_damage(
    record_move: RecordMove<'_>,
    [old_table, new_table]: [ViewLonghands<'_>; 2],
    font_lists_equal: bool,
    element_folds_transform_into_layout: bool,
    element_propagates_overflow_to_viewport: bool,
) -> StyleInvalidation {
    let old_record = record_move.old.values;
    let new_record = record_move.new.values;
    let old_values = record_move.old.computed_values();
    let new_values = record_move.new.computed_values();

    let all_groups_equal = old_record.payloads.holds_equal_payloads_as(new_record.payloads);
    let can_skip = all_groups_equal
        && old_table.same_record_as(new_table)
        && font_lists_equal
        && old_record.animated_overlay.is_none()
        && new_record.animated_overlay.is_none();
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
        if old_values.misc_interaction().implicit_will_change != new_values.misc_interaction().implicit_will_change {
            result.merge(property_invalidation(property_id::WILL_CHANGE, old_values, new_values));
        }
        // Equal resolved values can still inherit differently: currentcolor and an RGB color
        // may paint the same here, but resolve to different colors in an inheriting child.
        for (property, _) in old_table
            .inheritance_dependent_values()
            .chain(new_table.inheritance_dependent_values())
        {
            let (old, new) = (
                old_table.inheritance_dependent_value(property),
                new_table.inheritance_dependent_value(property),
            );
            // NB: Equal resolved colors can retain specified values that inherit differently.
            if old.map(RetainedStyleValueData::pointer) == new.map(RetainedStyleValueData::pointer)
                || old.map(RetainedStyleValueData::data) == new.map(RetainedStyleValueData::data)
            {
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
        // A longhand whose value both records hold in a group they share computes the same in
        // both, which only an animated value or a writing mode mapping it elsewhere would change.
        let shared = old_table.shared_with(new_table).filter(|_| {
            old_record.animated_overlay.is_none()
                && new_record.animated_overlay.is_none()
                && old_writing_mode == new_writing_mode
                && old_direction == new_direction
        });
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
            if shared.is_some_and(|shared| shared.holds(new_physical)) {
                continue;
            }
            let old_value = effective_value(&old_record, old_table, old_physical);
            let new_value = effective_value(&new_record, new_table, new_physical);
            if same_value(old_value, new_value) {
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
        let old_overlay = old_record.animated_overlay;
        let new_overlay = new_record.animated_overlay;
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
                let old_physical =
                    crate::css::style_compute::map_logical_alias_to_physical(property, old_writing_mode, old_direction);
                let new_physical =
                    crate::css::style_compute::map_logical_alias_to_physical(property, new_writing_mode, new_direction);
                if same_value(old_table.value(old_physical), new_table.value(new_physical)) {
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
    result
}

/// What moving an element, or one of its pseudo-elements, with `facts` between the records of
/// `record_move` damages. What the element's box was built with is left to the host: the counter
/// styles it resolved.
pub(crate) fn element_record_damage_of_views(facts: ElementDamageFacts, record_move: RecordMove<'_>) -> u32 {
    let old_values = record_move.old.computed_values();
    let new_values = record_move.new.computed_values();
    let stroke_uses_current_color = |values: ComputedValuesView<'_>| {
        let stroke = &values.inherited_svg().stroke;
        stroke.kind != SVG_PAINT_NONE && stroke.color.is_current_color
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
    // An absolutely positioned box takes its static position from where its display before box type transformation
    // would have placed it, so it can keep the same blockified display and still belong elsewhere among its siblings.
    let is_inline_level_absolutely_positioned = |values: ComputedValuesView<'_>| {
        values.is_absolutely_positioned() && values.display_before_box_type_transformation().is_inline_outside()
    };
    let font_lists_equal = old_values
        .font()
        .font_cascade_list
        .resolves_like(&new_values.font().font_cascade_list);
    let color_changed = old_values.inherited_text().color != new_values.inherited_text().color;
    let stroke_uses_current_color = stroke_uses_current_color(old_values) || stroke_uses_current_color(new_values);
    let table_fixup_child_changed = is_table_fixup_child(old_values) != is_table_fixup_child(new_values);
    let inline_level_absolutely_positioned_changed =
        is_inline_level_absolutely_positioned(old_values) != is_inline_level_absolutely_positioned(new_values);
    let overflow_changed =
        old_values.overflow_x() != new_values.overflow_x() || old_values.overflow_y() != new_values.overflow_y();
    let packed = compare_record_views(
        record_move,
        font_lists_equal,
        facts.folds_transform_into_layout,
        facts.propagates_overflow_to_viewport,
        facts.verifies_reads,
    );
    let mut damage = StyleInvalidation::unpack(packed);
    // Fieldsets with a rendered legend or flex contents transfer overflow to an anonymous
    // content box during tree construction. Rebuild to refresh both boxes' overrides.
    if overflow_changed && facts.is_field_set {
        damage.merge(StyleInvalidation::rebuild_layout_tree_from(REBUILD_ROOT_SELF));
    }
    // An SVG currentColor stroke stores its resolved color alongside the fact that it came from
    // currentColor. A color-only change can therefore alter the visible stroke width and the SVG
    // container bounds without changing the stroke longhand itself.
    if facts.is_svg_graphics_element && color_changed && stroke_uses_current_color {
        damage.ensure_level(INVALIDATION_RELAYOUT);
    }
    // Generated pseudo-element boxes are anonymous, so table fixup uses their adjusted display
    // instead.
    if !facts.is_pseudo_element && (table_fixup_child_changed || inline_level_absolutely_positioned_changed) {
        damage.any_computed_value_changed = true;
        damage.merge(StyleInvalidation::full());
    }
    damage.pack()
}

/// What moving a pseudo-element answers without reading its records: nothing when it stays on the
/// same record, and a repaint of a highlight pseudo-element, which generates no box.
fn pseudo_element_damage_without_records(pseudo_kind: u8, stays_on_its_record: bool) -> Option<u32> {
    if stays_on_its_record {
        return Some(0);
    }
    // NB: Highlight pseudo-elements do not generate boxes or affect layout.
    if super::publication::pseudo_kind::is_highlight(pseudo_kind) {
        return Some(
            StyleInvalidation {
                level: INVALIDATION_REPAINT,
                repaint_highlights: true,
                any_computed_value_changed: true,
                ..StyleInvalidation::default()
            }
            .pack(),
        );
    }
    None
}

/// What moving the pseudo-element of `pseudo_kind` of an element with `facts` between the records
/// of `record_move` damages. The originating element holds `originating`, which decides whether the
/// box stays inside the originating one. `counter_styles_changed` is the host's answer for what the
/// box was built with: the counter styles its generated content and marker resolved.
pub(crate) fn pseudo_element_record_damage_of_views(
    facts: ElementDamageFacts,
    pseudo_kind: u8,
    record_move: PseudoElementRecordMove<'_>,
    originating: RecordView<'_>,
    counter_styles_changed: bool,
) -> u32 {
    use super::publication::pseudo_kind::{AFTER, BEFORE, MARKER};
    let stays_on_its_record = match (record_move.old, record_move.new) {
        (None, None) => true,
        (Some(old), Some(new)) => old.identity.is_some() && old.identity == new.identity,
        _ => false,
    };
    if let Some(damage) = pseudo_element_damage_without_records(pseudo_kind, stays_on_its_record) {
        return damage;
    }
    let old_values = record_move.old.map(|record| record.computed_values());
    let new_values = record_move.new.map(|record| record.computed_values());
    let originating_values = originating.computed_values();

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

    let (Some(old), Some(new)) = (record_move.old, record_move.new) else {
        unreachable!("a box that appears or goes away is answered above");
    };
    let packed = element_record_damage_of_views(facts, RecordMove { old, new });
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
    damage.pack()
}

const _: () = assert!(FIRST_LONGHAND_PROPERTY_ID <= LAST_LONGHAND_PROPERTY_ID);

#[cfg(test)]
mod tests {
    use super::super::publication::pseudo_kind::BEFORE;
    use super::super::{StyleEngine, computed};
    use super::*;
    use crate::css::style_value::RetainedStyleValueData;
    use crate::css::table_group_builder::group_index;

    /// Install a record of the default groups over a longhand table holding `opacity` and the
    /// initial value of every other longhand for `target`, with `font_cascade_list` for its font
    /// cascade list.
    fn publish(
        engine: &mut StyleEngine,
        target: computed::ComputedStyleTarget,
        font_cascade_list: *const std::ffi::c_void,
        opacity: f64,
    ) -> u64 {
        let value_of = |property| match property {
            property_id::OPACITY => RetainedStyleValueData::from_owned(StyleValueData::Number { value: opacity }),
            _ => crate::css::style_compute::initial_value_handle(property).clone(),
        };
        // Comparing records reads their font cascade lists, which the default font group holds
        // none of. The test stubs compare lists by address.
        let mut payloads = engine.default_groups().groups().clone();
        // SAFETY: The font group holds a `FontValues`.
        unsafe {
            payloads.write_group::<crate::css::computed_value_types::FontValues>(group_index::FONT, |font| {
                font.font_cascade_list = libgfx_rust::font::FontCascadeListHandle::retain(font_cascade_list);
            });
        }
        let record = engine
            .install_test_record(target, Some(&value_of), Some(&payloads), 0)
            .raw();
        record
    }

    #[test]
    fn record_damage_read_from_views_matches_damage_read_by_identities() {
        let mut engine = StyleEngine::new();
        let mut raw_node = [0];
        engine.allocate_style_nodes(&mut raw_node);
        let node = StyleNodeID::from_raw(raw_node[0]).unwrap();
        let element = computed::ComputedStyleTarget::new(node, u8::MAX);
        let font_cascade_list = Box::new(0_u8);
        let font_cascade_list = (&raw const *font_cascade_list).cast();
        let old = publish(&mut engine, element, font_cascade_list, 1.0);
        let new = publish(&mut engine, element, font_cascade_list, 0.5);
        assert_ne!(old, new);

        let by_identities = engine.element_record_damage(node, false, old, new);
        let pseudo_by_identities = engine.pseudo_element_record_damage(node, BEFORE, old, new, new, false);
        let appearing_pseudo_by_identities = engine.pseudo_element_record_damage(node, BEFORE, 0, new, new, false);
        assert_ne!(by_identities, 0);

        // Views of records that are not installed read as installed ones do.
        let not_installed = |raw| RecordView {
            identity: None,
            ..RecordView::installed(&engine.computed_group_sets, raw).unwrap()
        };
        let record_move = RecordMove {
            old: not_installed(old),
            new: not_installed(new),
        };
        let by_views = element_record_damage_of_views(engine.element_damage_facts(node, false), record_move);
        assert_eq!(by_views, by_identities);

        let pseudo_facts = engine.element_damage_facts(node, true);
        let pseudo_by_views = pseudo_element_record_damage_of_views(
            pseudo_facts,
            BEFORE,
            PseudoElementRecordMove {
                old: Some(record_move.old),
                new: Some(record_move.new),
            },
            record_move.new,
            false,
        );
        assert_eq!(pseudo_by_views, pseudo_by_identities);
        let appearing_pseudo_by_views = pseudo_element_record_damage_of_views(
            pseudo_facts,
            BEFORE,
            PseudoElementRecordMove {
                old: None,
                new: Some(record_move.new),
            },
            record_move.new,
            false,
        );
        assert_eq!(appearing_pseudo_by_views, appearing_pseudo_by_identities);
    }

    #[test]
    fn a_color_change_repaints_a_decoration_of_current_color() {
        let mut engine = StyleEngine::new();
        let mut raw_node = [0];
        engine.allocate_style_nodes(&mut raw_node);
        let node = StyleNodeID::from_raw(raw_node[0]).unwrap();
        let element = computed::ComputedStyleTarget::new(node, u8::MAX);
        let mut publish_decorated = |color: u32, decoration_color| {
            let mut payloads = engine.default_groups().groups().clone();
            // SAFETY: The groups hold the values of their types.
            unsafe {
                payloads.write_group::<crate::css::computed_value_types::InheritedTextValues>(
                    group_index::INHERITED_TEXT,
                    |text| text.color = color,
                );
                payloads.write_group::<crate::css::computed_value_types::TextResetValues>(
                    group_index::TEXT_RESET,
                    |text| {
                        text.text_decoration_lines =
                            crate::css::computed_value_types::RetainedTextDecorationLineList::from_vec(vec![1]);
                        text.text_decoration_color = decoration_color;
                    },
                );
            }
            engine.install_test_record(element, None, Some(&payloads), 0).raw()
        };
        let current_color = crate::css::computed_value_types::StyleColor::CURRENT_COLOR;
        let blue = publish_decorated(0xff00_00ff, current_color);
        let red = publish_decorated(0xffff_0000, current_color);
        let view = |raw| {
            RecordView::installed(&engine.computed_group_sets, raw)
                .unwrap()
                .computed_values()
        };
        let invalidation = property_invalidation(property_id::COLOR, view(blue), view(red));
        assert!(invalidation.repaint_text_decorations);
        let unchanged = property_invalidation(property_id::COLOR, view(blue), view(blue));
        assert!(!unchanged.repaint_text_decorations);
    }
}
