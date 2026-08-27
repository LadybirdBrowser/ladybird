/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>

namespace Web::CSS {

static bool value_creates_stacking_context(PropertyID property_id, ComputedValues const& values)
{
    switch (property_id) {
    case PropertyID::Opacity:
        return values.opacity() < 1;
    case PropertyID::Transform:
        return values.has_transformations();
    case PropertyID::Translate:
        return values.has_translate();
    case PropertyID::Rotate:
        return values.has_rotate();
    case PropertyID::Scale:
        return values.has_scale();
    case PropertyID::Filter:
        return values.filter().has_filters();
    case PropertyID::BackdropFilter:
        return values.backdrop_filter().has_filters();
    case PropertyID::ClipPath:
        return values.clip_path().has_value();
    case PropertyID::Mask:
        return values.mask().has_value();
    case PropertyID::MaskImage:
        return values.mask_image() != nullptr;
    case PropertyID::ViewTransitionName:
        return values.view_transition_name().has_value();
    case PropertyID::Isolation:
        return values.isolation() == Isolation::Isolate;
    case PropertyID::MixBlendMode:
        return values.mix_blend_mode() != MixBlendMode::Normal;
    case PropertyID::ZIndex:
        return values.z_index().has_value();
    case PropertyID::Perspective:
        return values.perspective().has_value();
    case PropertyID::TransformStyle:
        return values.transform_style() != TransformStyle::Flat;
    case PropertyID::BackfaceVisibility:
        return values.backface_visibility() == BackfaceVisibility::Hidden;
    default:
        // For properties we haven't optimized (contain, container-type, will-change, all),
        // assume any value creates stacking context to be safe
        return true;
    }
}

// Mirrors the checks in Layout::Node::establishes_a_fixed_positioning_containing_block().
static bool value_establishes_containing_block(CSS::PropertyID property_id, ComputedValues const& values)
{
    switch (property_id) {
    case CSS::PropertyID::Transform:
        return values.has_transformations();
    case CSS::PropertyID::Translate:
        return values.has_translate();
    case CSS::PropertyID::Rotate:
        return values.has_rotate();
    case CSS::PropertyID::Scale:
        return values.has_scale();
    case CSS::PropertyID::Perspective:
        return values.perspective().has_value();
    case CSS::PropertyID::TransformStyle:
        return values.transform_style() == CSS::TransformStyle::Preserve3d;
    case CSS::PropertyID::BackfaceVisibility:
        return values.backface_visibility() == CSS::BackfaceVisibility::Hidden;
    case CSS::PropertyID::Filter:
        return values.filter().has_filters();
    case CSS::PropertyID::BackdropFilter:
        return values.backdrop_filter().has_filters();
    case CSS::PropertyID::Contain:
        return values.contain().layout_containment || values.contain().paint_containment;
    case CSS::PropertyID::WillChange: {
        // will-change establishes a containing block only when it mentions a property whose
        // non-initial value would, or `position`, which additionally lets a non-atomic inline
        // establish an absolute positioning containing block.
        auto will_change = values.will_change();
        for (auto const& entry : will_change.entries()) {
            if (!entry.has<PropertyID>())
                continue;
            auto property = entry.get<PropertyID>();
            if (AK::first_is_one_of(property,
                    CSS::PropertyID::Transform, CSS::PropertyID::Translate, CSS::PropertyID::Rotate,
                    CSS::PropertyID::Scale, CSS::PropertyID::Perspective, CSS::PropertyID::TransformStyle,
                    CSS::PropertyID::BackfaceVisibility, CSS::PropertyID::Filter, CSS::PropertyID::BackdropFilter,
                    CSS::PropertyID::Contain, CSS::PropertyID::Position))
                return true;
        }
        return false;
    }
    case CSS::PropertyID::ContainerType:
        // container-type: size and inline-size apply layout containment; normal does not.
        return values.container_type().is_size_container || values.container_type().is_inline_size_container;
    default:
        return false;
    }
}

static bool animated_value_creates_stacking_context(PropertyID property_id, StyleValue const* value)
{
    if (!value)
        return false;
    switch (property_id) {
    case PropertyID::Opacity:
        return value->as_opacity_value().resolved() < 1;
    case PropertyID::Transform:
        return value->to_keyword() != Keyword::None
            && (!value->is_value_list() || !value->as_value_list().values().is_empty());
    case PropertyID::Translate:
    case PropertyID::Rotate:
    case PropertyID::Scale:
    case PropertyID::ClipPath:
    case PropertyID::Mask:
    case PropertyID::MaskImage:
    case PropertyID::ViewTransitionName:
        return value->to_keyword() != Keyword::None;
    case PropertyID::Filter:
    case PropertyID::BackdropFilter:
        return value->to_keyword() != Keyword::None
            && (!value->is_value_list() || !value->as_value_list().values().is_empty());
    case PropertyID::Isolation:
        return value->to_keyword() == Keyword::Isolate;
    case PropertyID::MixBlendMode:
        return value->to_keyword() != Keyword::Normal;
    case PropertyID::ZIndex:
        return value->to_keyword() != Keyword::Auto;
    case PropertyID::Perspective:
    case PropertyID::TransformStyle:
        return value->to_keyword() != Keyword::None && value->to_keyword() != Keyword::Flat;
    case PropertyID::BackfaceVisibility:
        return value->to_keyword() == Keyword::Hidden;
    default:
        return true;
    }
}

static bool animated_value_establishes_containing_block(PropertyID property_id, StyleValue const* value)
{
    if (!value)
        return false;
    switch (property_id) {
    case PropertyID::Transform:
        return value->to_keyword() != Keyword::None
            && (!value->is_value_list() || !value->as_value_list().values().is_empty());
    case PropertyID::Translate:
    case PropertyID::Rotate:
    case PropertyID::Scale:
    case PropertyID::Perspective:
        return value->to_keyword() != Keyword::None;
    case PropertyID::TransformStyle:
        return value->to_keyword() == Keyword::Preserve3d;
    case PropertyID::BackfaceVisibility:
        return value->to_keyword() == Keyword::Hidden;
    case PropertyID::Filter:
    case PropertyID::BackdropFilter:
        return value->to_keyword() != Keyword::None;
    case PropertyID::Contain: {
        auto establishes = [](Keyword keyword) {
            return AK::first_is_one_of(keyword, Keyword::Strict, Keyword::Content, Keyword::Layout, Keyword::Paint);
        };
        if (value->is_keyword())
            return establishes(value->to_keyword());
        if (!value->is_value_list())
            return true;
        return any_of(value->as_value_list().values(), [&](auto const& entry) {
            return entry->is_keyword() && establishes(entry->to_keyword());
        });
    }
    case PropertyID::WillChange:
        if (value->to_keyword() == Keyword::Auto)
            return false;
        if (!value->is_value_list())
            return false;
        return any_of(value->as_value_list().values(), [](auto const& entry) {
            if (!entry->is_custom_ident())
                return false;
            auto property = property_id_from_string(entry->as_custom_ident().custom_ident());
            return property.has_value() && AK::first_is_one_of(*property, PropertyID::Transform, PropertyID::Translate, PropertyID::Rotate, PropertyID::Scale, PropertyID::Perspective, PropertyID::TransformStyle, PropertyID::BackfaceVisibility, PropertyID::Filter, PropertyID::BackdropFilter, PropertyID::Contain, PropertyID::Position);
        });
    case PropertyID::ContainerType:
        return value->to_keyword() == Keyword::Size || value->to_keyword() == Keyword::InlineSize;
    default:
        return false;
    }
}

static Optional<bool> animated_transform_value_is_invertible(StyleValue const* value)
{
    if (!value || value->to_keyword() == Keyword::None)
        return true;
    auto is_invertible = [](TransformationStyleValue const& transformation) -> Optional<bool> {
        if (!transformation.can_be_converted_to_matrix_without_reference_box())
            return {};
        return transformation.to_matrix({}).is_invertible();
    };
    if (value->is_transformation())
        return is_invertible(value->as_transformation());
    if (value->is_value_list()) {
        auto matrix = Gfx::FloatMatrix4x4::identity();
        for (auto const& transformation : value->as_value_list().values()) {
            if (!transformation->is_transformation() || !transformation->as_transformation().can_be_converted_to_matrix_without_reference_box())
                return {};
            matrix = matrix * transformation->as_transformation().to_matrix({});
        }
        return matrix.is_invertible();
    }
    return {};
}

static bool opacity_change_affects_paintable_visibility(CSS::PropertyID property_id, ComputedValues const& old_values, ComputedValues const& new_values)
{
    if (property_id != CSS::PropertyID::Opacity)
        return false;
    return (old_values.opacity() == 0.0f) != (new_values.opacity() == 0.0f);
}

static Optional<bool> transform_value_is_invertible(PropertyID property_id, ComputedValues const& values)
{
    if (property_id == PropertyID::Scale) {
        if (!values.has_scale())
            return true;
        if (!values.scale()->can_be_converted_to_matrix_without_reference_box())
            return {};
        return values.scale()->to_matrix({}).is_invertible();
    }
    if (property_id == PropertyID::Transform) {
        auto matrix = Gfx::FloatMatrix4x4::identity();
        bool can_be_converted_without_reference_box = true;
        values.for_each_transformation([&](auto const& transformation) {
            if (!transformation.can_be_converted_to_matrix_without_reference_box()) {
                can_be_converted_without_reference_box = false;
                return;
            }
            matrix = matrix * transformation.to_matrix({});
        });
        if (!can_be_converted_without_reference_box)
            return {};
        return matrix.is_invertible();
    }
    return {};
}

static bool transform_change_requires_repaint(CSS::PropertyID property_id, ComputedValues const& old_values, ComputedValues const& new_values)
{
    if (!AK::first_is_one_of(property_id, CSS::PropertyID::Transform, CSS::PropertyID::Scale))
        return false;

    // StackingContext::paint() omits non-invertibly transformed subtrees, so crossing
    // this boundary changes display-list contents, not just the visual context matrix.
    auto old_invertible = transform_value_is_invertible(property_id, old_values);
    auto new_invertible = transform_value_is_invertible(property_id, new_values);
    if (!old_invertible.has_value() || !new_invertible.has_value())
        return true;
    return old_invertible.value() != new_invertible.value();
}

static bool accumulated_visual_context_property_always_requires_repaint(CSS::PropertyID property_id)
{
    switch (property_id) {
    case CSS::PropertyID::BackdropFilter:
    case CSS::PropertyID::BackgroundAttachment:
    case CSS::PropertyID::BackgroundBlendMode:
    case CSS::PropertyID::BackgroundClip:
    case CSS::PropertyID::BackgroundImage:
    case CSS::PropertyID::BorderBottomLeftRadius:
    case CSS::PropertyID::BorderBottomRightRadius:
    case CSS::PropertyID::BorderBottomStyle:
    case CSS::PropertyID::BorderLeftStyle:
    case CSS::PropertyID::BorderRightStyle:
    case CSS::PropertyID::BorderTopLeftRadius:
    case CSS::PropertyID::BorderTopRightRadius:
    case CSS::PropertyID::BorderTopStyle:
    case CSS::PropertyID::BoxShadow:
    case CSS::PropertyID::Clip:
    case CSS::PropertyID::ClipPath:
    case CSS::PropertyID::MaskClip:
    case CSS::PropertyID::MaskComposite:
    case CSS::PropertyID::MaskImage:
    case CSS::PropertyID::MaskType:
    case CSS::PropertyID::MixBlendMode:
    case CSS::PropertyID::OutlineOffset:
    case CSS::PropertyID::OutlineStyle:
    case CSS::PropertyID::OutlineWidth:
    case CSS::PropertyID::Perspective:
        return true;
    default:
        return false;
    }
}

static bool accumulated_visual_context_change_requires_repaint(CSS::PropertyID property_id, ComputedValues const& old_values, ComputedValues const& new_values)
{
    if (opacity_change_affects_paintable_visibility(property_id, old_values, new_values))
        return true;

    if (transform_change_requires_repaint(property_id, old_values, new_values))
        return true;

    return accumulated_visual_context_property_always_requires_repaint(property_id);
}

// Whether this change only affects values carried by existing accumulated visual context nodes, so they can be
// patched in place. Presence flips (e.g. transform none <-> non-none) change the tree structure and which boxes
// establish abs/fixed positioning containing blocks, and must rebuild instead.
// NB: Must only include properties whose data update_accumulated_visual_context_values() recomputes.
static bool accumulated_visual_context_change_is_value_only(CSS::PropertyID property_id, ComputedValues const& old_values, ComputedValues const& new_values)
{
    switch (property_id) {
    case CSS::PropertyID::TransformOrigin:
    case CSS::PropertyID::TransformBox:
    case CSS::PropertyID::PerspectiveOrigin:
        // Origins and the reference box never affect node presence.
        return true;
    case CSS::PropertyID::Transform:
    case CSS::PropertyID::Translate:
    case CSS::PropertyID::Rotate:
    case CSS::PropertyID::Scale:
    case CSS::PropertyID::Opacity:
    case CSS::PropertyID::Filter:
    case CSS::PropertyID::MixBlendMode:
    case CSS::PropertyID::Perspective:
        // Value-only when the property contributes a node both before and after the change.
        return value_creates_stacking_context(property_id, old_values)
            && value_creates_stacking_context(property_id, new_values);
    default:
        return false;
    }
}

RequiredInvalidationAfterStyleChange compute_property_invalidation(CSS::PropertyID property_id, ComputedValues const& old_computed_values, ComputedValues const& new_computed_values)
{
    RequiredInvalidationAfterStyleChange invalidation;

    // Entering or leaving out-of-flow or floated layout can restructure runs of inline content around the element,
    // even when its adjusted outer display type remains block.
    if (property_id == CSS::PropertyID::Position) {
        auto is_out_of_flow = [](CSS::Positioning position) {
            return position == CSS::Positioning::Absolute || position == CSS::Positioning::Fixed;
        };
        if (is_out_of_flow(old_computed_values.position()) != is_out_of_flow(new_computed_values.position()))
            return RequiredInvalidationAfterStyleChange::full();
    }
    if (property_id == CSS::PropertyID::Float) {
        if ((old_computed_values.float_() == CSS::Float::None) != (new_computed_values.float_() == CSS::Float::None))
            return RequiredInvalidationAfterStyleChange::full();
    }

    // Other display, float, and position changes which preserve the outer display type cannot change whether the
    // element participates in inline or block layout among its siblings. Its principal box can be replaced in place
    // while rebuilding its descendants for the new inner type.
    if (AK::first_is_one_of(property_id, CSS::PropertyID::Display, CSS::PropertyID::Float, CSS::PropertyID::Position)) {
        auto old_display = old_computed_values.display();
        auto new_display = new_computed_values.display();
        if (old_display.is_outside_and_inside()
            && new_display.is_outside_and_inside()
            && old_display.outside() == new_display.outside())
            return RequiredInvalidationAfterStyleChange::rebuild_layout_tree_from(LayoutTreeRebuildRoot::Self);
    }

    // These properties only change the contents of the element's own layout subtree.
    if (AK::first_is_one_of(property_id, CSS::PropertyID::Content, CSS::PropertyID::ContentVisibility, CSS::PropertyID::TextTransform))
        return RequiredInvalidationAfterStyleChange::rebuild_layout_tree_from(LayoutTreeRebuildRoot::Self);

    // Marker boxes and their text are generated during layout tree construction and belong to the item's own subtree.
    if (AK::first_is_one_of(property_id, CSS::PropertyID::ListStyleType, CSS::PropertyID::ListStyleImage, CSS::PropertyID::ListStylePosition)
        && (old_computed_values.display().is_list_item() || new_computed_values.display().is_list_item()))
        return RequiredInvalidationAfterStyleChange::rebuild_layout_tree_from(LayoutTreeRebuildRoot::Self);

    // A box that appears or disappears leaves every sibling box intact, so the parent can treat the
    // change like a child-list mutation instead of rebuilding its whole subtree.
    if (property_id == CSS::PropertyID::Display) {
        auto old_display = old_computed_values.display();
        auto new_display = new_computed_values.display();
        if (old_display.is_none() != new_display.is_none()) {
            auto const& box_display = old_display.is_none() ? new_display : old_display;
            if (box_display.is_outside_and_inside())
                return RequiredInvalidationAfterStyleChange::rebuild_layout_tree_from(LayoutTreeRebuildRoot::BoxPresenceChange);
        }
    }

    // NB: Other display, float, or position changes have to rebuild from the parent.
    if (AK::first_is_one_of(property_id, CSS::PropertyID::Display, CSS::PropertyID::Float, CSS::PropertyID::Position)) {
        return RequiredInvalidationAfterStyleChange::full();
    }

    // Overflow propagation from the document element or body reaches the viewport. Other overflow changes are
    // confined to the element's own subtree.
    if (property_id == CSS::PropertyID::OverflowX || property_id == CSS::PropertyID::OverflowY) {
        return RequiredInvalidationAfterStyleChange::rebuild_layout_tree_from(LayoutTreeRebuildRoot::SelfUnlessDocumentElementOrBody);
    }

    if (AK::first_is_one_of(property_id, CSS::PropertyID::CounterReset, CSS::PropertyID::CounterSet, CSS::PropertyID::CounterIncrement)) {
        invalidation.ensure_at_least(InvalidationLevel::RebuildLayoutTree);
        return invalidation;
    }

    if (AK::first_is_one_of(property_id, CSS::PropertyID::ContainerName, CSS::PropertyID::ContainerType))
        invalidation.recompute_descendant_styles = true;

    // Text decorations propagate to descendant boxes, which paint them from this element's computed
    // values, so their cached paint commands must be discarded even though their style is unchanged.
    // No decoration is painted from a box whose text-decoration-line is empty before and after the
    // change, so such boxes are skipped.
    if (property_id == CSS::PropertyID::TextDecorationLine) {
        invalidation.repaint_propagated_text_decorations = true;
    } else if (AK::first_is_one_of(property_id, CSS::PropertyID::TextDecorationColor,
                   CSS::PropertyID::TextDecorationStyle, CSS::PropertyID::TextDecorationThickness,
                   CSS::PropertyID::TextUnderlineOffset, CSS::PropertyID::TextUnderlinePosition,
                   CSS::PropertyID::Color)) {
        if (!old_computed_values.text_decoration_line().is_empty()
            || !new_computed_values.text_decoration_line().is_empty()) {
            if (property_id == CSS::PropertyID::Color) {
                // A text-decoration-color of currentcolor is stored resolved, so a color change can move
                // the painted decoration color without any text-decoration property changing.
                invalidation.repaint_propagated_text_decorations = old_computed_values.text_decoration_color() != new_computed_values.text_decoration_color();
            } else {
                invalidation.repaint_propagated_text_decorations = true;
            }
        }
    }

    // https://drafts.csswg.org/css-logical-1/#intro
    // This mapping, which depends on the used values of writing-mode, direction, and text-orientation, controls the
    // interpretation of flow-relative keywords and properties.
    // NB: Since writing-mode and direction are inherited, changing either on an ancestor requires recascading its
    //     descendants.
    // FIXME: Include text-orientation once it is implemented.
    if (AK::first_is_one_of(property_id, CSS::PropertyID::Direction, CSS::PropertyID::WritingMode))
        invalidation.recompute_descendant_styles = true;

    // OPTIMIZATION: Special handling for CSS `visibility`:
    if (property_id == CSS::PropertyID::Visibility) {
        // We don't need to relayout if the visibility changes from visible to hidden or vice versa. Only collapse requires relayout.
        auto old_is_collapsed = old_computed_values.visibility() == Visibility::Collapse;
        auto new_is_collapsed = new_computed_values.visibility() == Visibility::Collapse;
        if (old_is_collapsed != new_is_collapsed)
            invalidation.ensure_at_least(InvalidationLevel::Relayout);
        // Of course, we still have to repaint on any visibility change.
        invalidation.ensure_at_least(InvalidationLevel::Repaint);
    } else if (CSS::property_affects_layout(property_id)) {
        invalidation.ensure_at_least(InvalidationLevel::Relayout);
    }

    // Scrollable overflow depends on these properties even though layout does not: a scroll container's
    // scrollable overflow rect includes descendant border boxes as projected by their transforms.
    if (CSS::property_affects_scrollable_overflow(property_id))
        invalidation.set_needs_scrollable_overflow_recalculation();

    // https://drafts.csswg.org/css-scroll-snap-1/#re-snap
    // NB: A scroll snap property change moves the snap positions of a snap container without necessarily changing
    //     layout, so snap containers re-evaluate their scroll position for these properties as they do after a
    //     layout change.
    if (CSS::property_affects_scrollable_overflow(property_id)
        || AK::first_is_one_of(property_id,
            CSS::PropertyID::ScrollSnapType, CSS::PropertyID::ScrollSnapAlign, CSS::PropertyID::ScrollSnapStop,
            CSS::PropertyID::ScrollMarginTop, CSS::PropertyID::ScrollMarginRight, CSS::PropertyID::ScrollMarginBottom, CSS::PropertyID::ScrollMarginLeft,
            CSS::PropertyID::ScrollPaddingTop, CSS::PropertyID::ScrollPaddingRight, CSS::PropertyID::ScrollPaddingBottom, CSS::PropertyID::ScrollPaddingLeft)) {
        invalidation.needs_scroll_container_resnap = true;
    }

    if (CSS::property_affects_stacking_context(property_id)) {
        // z-index changes always require rebuilding the stacking context tree because
        // the value determines painting order within the tree, not just whether a
        // stacking context is created. During tree construction, elements with
        // z-index 0/auto are placed in m_positioned_descendants_and_stacking_contexts_
        // with_stack_level_0, while elements with non-zero z-index are painted from
        // m_children (negative z-index at step 3, positive at step 9 of CSS 2.1
        // Appendix E). If z-index changes between non-auto values (e.g. 0 -> 10),
        // both old and new values create stacking contexts, so the generic optimization
        // below would skip the rebuild. But the element remains in the wrong list,
        // causing it to be painted from both step 8 (m_positioned_descendants) and
        // step 9 (m_children with z >= 1), resulting in double painting.
        if (property_id == CSS::PropertyID::ZIndex) {
            invalidation.set_needs_stacking_context_tree_rebuild();
        } else {
            // OPTIMIZATION: Only rebuild stacking context tree when property crosses from a neutral value (doesn't create
            //               stacking context) to a creating value or vice versa.
            bool old_creates = value_creates_stacking_context(property_id, old_computed_values);
            bool new_creates = value_creates_stacking_context(property_id, new_computed_values);
            if (old_creates != new_creates) {
                invalidation.set_needs_stacking_context_tree_rebuild();
            }
        }
    }

    if (value_establishes_containing_block(property_id, old_computed_values) != value_establishes_containing_block(property_id, new_computed_values))
        invalidation.changes_containing_block_establishment = true;

    // A grouping property value forces a used transform-style of flat, which changes whether descendants with
    // backface-visibility: hidden participate in a 3D rendering context and establish containing blocks.
    // Containing block pointers are only recomputed by a layout pass.
    if (new_computed_values.transform_style() == CSS::TransformStyle::Preserve3d
        && old_computed_values.has_transform_style_grouping_property() != new_computed_values.has_transform_style_grouping_property()) {
        invalidation.changes_containing_block_establishment = true;
        invalidation.ensure_at_least(InvalidationLevel::Relayout);
    }

    bool needs_repaint = true;
    if (CSS::property_affects_accumulated_visual_contexts(property_id)) {
        if (accumulated_visual_context_change_is_value_only(property_id, old_computed_values, new_computed_values))
            invalidation.ensure_at_least(AccumulatedVisualContextInvalidation::UpdateValues);
        else
            invalidation.ensure_at_least(AccumulatedVisualContextInvalidation::Rebuild);
        if (!accumulated_visual_context_change_requires_repaint(property_id, old_computed_values, new_computed_values)
            && !invalidation.needs_repaint()
            && !invalidation.recompute_descendant_styles)
            needs_repaint = false;
    }
    if (needs_repaint)
        invalidation.ensure_at_least(InvalidationLevel::Repaint);

    return invalidation;
}

RequiredInvalidationAfterStyleChange compute_property_invalidation(CSS::PropertyID property_id, StyleValue const* old_value, StyleValue const* new_value)
{
    RequiredInvalidationAfterStyleChange invalidation;
    if (old_value == new_value || (old_value && new_value && old_value->equals(*new_value)))
        return invalidation;

    // Animated display, float and position changes are recomputed against a complete base style
    // by the caller. Conservatively rebuild until that typed style is available.
    if (AK::first_is_one_of(property_id, PropertyID::Display, PropertyID::Float, PropertyID::Position))
        return RequiredInvalidationAfterStyleChange::full();
    if (AK::first_is_one_of(property_id, PropertyID::Content, PropertyID::ContentVisibility, PropertyID::TextTransform))
        return RequiredInvalidationAfterStyleChange::rebuild_layout_tree_from(LayoutTreeRebuildRoot::Self);
    if (AK::first_is_one_of(property_id, PropertyID::ListStyleType, PropertyID::ListStyleImage, PropertyID::ListStylePosition))
        return RequiredInvalidationAfterStyleChange::rebuild_layout_tree_from(LayoutTreeRebuildRoot::Self);
    if (property_id == PropertyID::OverflowX || property_id == PropertyID::OverflowY)
        return RequiredInvalidationAfterStyleChange::rebuild_layout_tree_from(LayoutTreeRebuildRoot::SelfUnlessDocumentElementOrBody);
    if (AK::first_is_one_of(property_id, PropertyID::CounterReset, PropertyID::CounterSet, PropertyID::CounterIncrement)) {
        invalidation.ensure_at_least(InvalidationLevel::RebuildLayoutTree);
        return invalidation;
    }
    if (AK::first_is_one_of(property_id, PropertyID::ContainerName, PropertyID::ContainerType))
        invalidation.recompute_descendant_styles = true;

    if (property_id == PropertyID::TextDecorationLine) {
        invalidation.repaint_propagated_text_decorations = true;
    } else if (AK::first_is_one_of(property_id, PropertyID::TextDecorationColor,
                   PropertyID::TextDecorationStyle, PropertyID::TextDecorationThickness,
                   PropertyID::TextUnderlineOffset, PropertyID::TextUnderlinePosition,
                   PropertyID::Color)) {
        invalidation.repaint_propagated_text_decorations = true;
    }

    if (property_id == PropertyID::Visibility) {
        auto old_is_collapsed = old_value && old_value->to_keyword() == Keyword::Collapse;
        auto new_is_collapsed = new_value && new_value->to_keyword() == Keyword::Collapse;
        if (old_is_collapsed != new_is_collapsed)
            invalidation.ensure_at_least(InvalidationLevel::Relayout);
        invalidation.ensure_at_least(InvalidationLevel::Repaint);
    } else if (property_affects_layout(property_id)) {
        invalidation.ensure_at_least(InvalidationLevel::Relayout);
    }

    if (property_affects_scrollable_overflow(property_id))
        invalidation.set_needs_scrollable_overflow_recalculation();
    if (property_affects_stacking_context(property_id)) {
        if (property_id == PropertyID::ZIndex
            || animated_value_creates_stacking_context(property_id, old_value) != animated_value_creates_stacking_context(property_id, new_value))
            invalidation.set_needs_stacking_context_tree_rebuild();
    }
    if (animated_value_establishes_containing_block(property_id, old_value) != animated_value_establishes_containing_block(property_id, new_value))
        invalidation.changes_containing_block_establishment = true;

    bool needs_repaint = true;
    if (property_affects_accumulated_visual_contexts(property_id)) {
        bool value_only = AK::first_is_one_of(property_id, PropertyID::TransformOrigin, PropertyID::TransformBox, PropertyID::PerspectiveOrigin)
            || (AK::first_is_one_of(property_id, PropertyID::Transform, PropertyID::Translate, PropertyID::Rotate,
                    PropertyID::Scale, PropertyID::Opacity, PropertyID::Filter, PropertyID::MixBlendMode, PropertyID::Perspective)
                && animated_value_creates_stacking_context(property_id, old_value)
                && animated_value_creates_stacking_context(property_id, new_value));
        invalidation.ensure_at_least(value_only ? AccumulatedVisualContextInvalidation::UpdateValues : AccumulatedVisualContextInvalidation::Rebuild);

        bool requires_repaint = false;
        if (property_id == PropertyID::Opacity) {
            auto old_opacity = old_value ? old_value->as_opacity_value().resolved() : 1.0f;
            auto new_opacity = new_value ? new_value->as_opacity_value().resolved() : 1.0f;
            requires_repaint = (old_opacity == 0.0f) != (new_opacity == 0.0f);
        } else if (AK::first_is_one_of(property_id, PropertyID::Transform, PropertyID::Scale)) {
            auto old_invertible = animated_transform_value_is_invertible(old_value);
            auto new_invertible = animated_transform_value_is_invertible(new_value);
            requires_repaint = !old_invertible.has_value() || !new_invertible.has_value() || old_invertible.value() != new_invertible.value();
        } else if (accumulated_visual_context_property_always_requires_repaint(property_id)) {
            requires_repaint = true;
        }
        if (!requires_repaint && !invalidation.needs_repaint() && !invalidation.recompute_descendant_styles)
            needs_repaint = false;
    }
    if (needs_repaint)
        invalidation.ensure_at_least(InvalidationLevel::Repaint);
    return invalidation;
}

}
