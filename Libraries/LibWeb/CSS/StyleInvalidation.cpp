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
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>

namespace Web::CSS {

static Optional<bool> typed_value_creates_stacking_context(PropertyID property_id, ComputedValues const& values)
{
    switch (property_id) {
    case PropertyID::Opacity:
        return values.opacity() < 1;
    case PropertyID::Transform:
        return !values.transformations().is_empty();
    case PropertyID::Translate:
        return values.translate() != nullptr;
    case PropertyID::Rotate:
        return values.rotate() != nullptr;
    case PropertyID::Scale:
        return values.scale() != nullptr;
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
    default:
        return {};
    }
}

static bool is_stacking_context_creating_value(CSS::PropertyID property_id, StyleValue const* value, ComputedValues const* computed_values = nullptr)
{
    if (computed_values) {
        if (auto result = typed_value_creates_stacking_context(property_id, *computed_values); result.has_value())
            return result.value();
    }
    if (!value)
        return false;

    switch (property_id) {
    case CSS::PropertyID::Opacity:
        return value->as_opacity_value().resolved() < 1;
    case CSS::PropertyID::Transform:
        if (value->to_keyword() == CSS::Keyword::None)
            return false;
        if (value->is_value_list())
            return value->as_value_list().size() > 0;
        return value->is_transformation();
    case CSS::PropertyID::Translate:
    case CSS::PropertyID::Rotate:
    case CSS::PropertyID::Scale:
        return value->to_keyword() != CSS::Keyword::None;
    case CSS::PropertyID::Filter:
    case CSS::PropertyID::BackdropFilter:
        if (value->is_keyword())
            return value->to_keyword() != CSS::Keyword::None;
        return is_filter_style_value_list(*value) && value->as_value_list().size() > 0;
    case CSS::PropertyID::ClipPath:
    case CSS::PropertyID::Mask:
    case CSS::PropertyID::MaskImage:
    case CSS::PropertyID::ViewTransitionName:
        return value->to_keyword() != CSS::Keyword::None;
    case CSS::PropertyID::Isolation:
        return value->to_keyword() == CSS::Keyword::Isolate;
    case CSS::PropertyID::MixBlendMode:
        return value->to_keyword() != CSS::Keyword::Normal;
    case CSS::PropertyID::ZIndex:
        return value->to_keyword() != CSS::Keyword::Auto;
    case CSS::PropertyID::Perspective:
    case CSS::PropertyID::TransformStyle:
        return value->to_keyword() != CSS::Keyword::None && value->to_keyword() != CSS::Keyword::Flat;
    default:
        // For properties we haven't optimized (contain, container-type, will-change, all),
        // assume any value creates stacking context to be safe
        return true;
    }
}

// Mirrors the checks in Layout::Node::establishes_a_fixed_positioning_containing_block().
static bool is_containing_block_establishing_value(CSS::PropertyID property_id, StyleValue const* value)
{
    if (!value)
        return false;

    switch (property_id) {
    case CSS::PropertyID::Transform:
        if (value->to_keyword() == CSS::Keyword::None)
            return false;
        if (value->is_value_list())
            return value->as_value_list().size() > 0;
        return value->is_transformation();
    case CSS::PropertyID::Translate:
    case CSS::PropertyID::Rotate:
    case CSS::PropertyID::Scale:
    case CSS::PropertyID::Perspective:
        return value->to_keyword() != CSS::Keyword::None;
    case CSS::PropertyID::TransformStyle:
        return value->to_keyword() == CSS::Keyword::Preserve3d;
    case CSS::PropertyID::Filter:
    case CSS::PropertyID::BackdropFilter:
        if (value->is_keyword())
            return value->to_keyword() != CSS::Keyword::None;
        return is_filter_style_value_list(*value);
    case CSS::PropertyID::Contain: {
        // contain: none | strict | content | [ size || inline-size || layout || style || paint ]
        // Only layout and paint containment (which strict and content include) establish a
        // containing block; size, inline-size and style containment do not.
        auto keyword_establishes_containing_block = [](CSS::Keyword keyword) {
            return AK::first_is_one_of(keyword, CSS::Keyword::Strict, CSS::Keyword::Content, CSS::Keyword::Layout, CSS::Keyword::Paint);
        };
        if (value->is_keyword())
            return keyword_establishes_containing_block(value->to_keyword());
        if (value->is_value_list()) {
            for (auto const& entry : value->as_value_list().values()) {
                if (entry->is_keyword() && keyword_establishes_containing_block(entry->to_keyword()))
                    return true;
            }
            return false;
        }
        return true;
    }
    case CSS::PropertyID::WillChange: {
        // will-change establishes a containing block only when it mentions a property whose
        // non-initial value would, or `position`, which additionally lets a non-atomic inline
        // establish an absolute positioning containing block.
        auto entry_establishes_containing_block = [](StyleValue const& entry) {
            if (!entry.is_custom_ident())
                return false;
            auto property = property_id_from_string(entry.as_custom_ident().custom_ident());
            if (!property.has_value())
                return false;
            return AK::first_is_one_of(*property,
                CSS::PropertyID::Transform, CSS::PropertyID::Translate, CSS::PropertyID::Rotate,
                CSS::PropertyID::Scale, CSS::PropertyID::Perspective, CSS::PropertyID::TransformStyle,
                CSS::PropertyID::Filter, CSS::PropertyID::BackdropFilter, CSS::PropertyID::Contain,
                CSS::PropertyID::Position);
        };
        if (value->to_keyword() == CSS::Keyword::Auto)
            return false;
        if (value->is_value_list()) {
            for (auto const& entry : value->as_value_list().values()) {
                if (entry_establishes_containing_block(entry))
                    return true;
            }
            return false;
        }
        return entry_establishes_containing_block(*value);
    }
    case CSS::PropertyID::ContainerType:
        // container-type: size and inline-size apply layout containment; normal does not.
        return value->to_keyword() == CSS::Keyword::Size || value->to_keyword() == CSS::Keyword::InlineSize;
    default:
        return false;
    }
}

static bool opacity_change_affects_paintable_visibility(CSS::PropertyID property_id, StyleValue const* old_value, StyleValue const* new_value, ComputedValues const* old_computed_values, ComputedValues const* new_computed_values)
{
    if (property_id != CSS::PropertyID::Opacity)
        return false;

    auto old_opacity = old_computed_values ? old_computed_values->opacity() : old_value ? old_value->as_opacity_value().resolved()
                                                                                        : 1.0f;
    auto new_opacity = new_computed_values ? new_computed_values->opacity() : new_value ? new_value->as_opacity_value().resolved()
                                                                                        : 1.0f;
    return (old_opacity == 0.0f) != (new_opacity == 0.0f);
}

static Optional<bool> transform_value_is_invertible(PropertyID property_id, StyleValue const* value, ComputedValues const* computed_values)
{
    if (computed_values) {
        if (property_id == PropertyID::Scale) {
            if (!computed_values->scale())
                return true;
            if (!computed_values->scale()->can_be_converted_to_matrix_without_reference_box())
                return {};
            return computed_values->scale()->to_matrix({}).is_invertible();
        }
        if (property_id == PropertyID::Transform) {
            auto matrix = Gfx::FloatMatrix4x4::identity();
            for (auto const& transformation : computed_values->transformations()) {
                if (!transformation->can_be_converted_to_matrix_without_reference_box())
                    return {};
                matrix = matrix * transformation->to_matrix({});
            }
            return matrix.is_invertible();
        }
    }

    if (!value || value->to_keyword() == CSS::Keyword::None)
        return true;

    auto transformation_is_invertible = [](TransformationStyleValue const& transformation) -> Optional<bool> {
        if (!transformation.can_be_converted_to_matrix_without_reference_box())
            return {};
        return transformation.to_matrix({}).is_invertible();
    };

    if (value->is_transformation())
        return transformation_is_invertible(value->as_transformation());

    if (value->is_value_list()) {
        auto matrix = Gfx::FloatMatrix4x4::identity();
        for (auto const& transformation : value->as_value_list().values()) {
            if (!transformation->is_transformation())
                return {};
            if (!transformation->as_transformation().can_be_converted_to_matrix_without_reference_box())
                return {};
            matrix = matrix * transformation->as_transformation().to_matrix({});
        }
        return matrix.is_invertible();
    }

    return {};
}

static bool transform_change_requires_repaint(CSS::PropertyID property_id, StyleValue const* old_value, StyleValue const* new_value, ComputedValues const* old_computed_values, ComputedValues const* new_computed_values)
{
    if (!AK::first_is_one_of(property_id, CSS::PropertyID::Transform, CSS::PropertyID::Scale))
        return false;

    // StackingContext::paint() omits non-invertibly transformed subtrees, so crossing
    // this boundary changes display-list contents, not just the visual context matrix.
    auto old_invertible = transform_value_is_invertible(property_id, old_value, old_computed_values);
    auto new_invertible = transform_value_is_invertible(property_id, new_value, new_computed_values);
    if (!old_invertible.has_value() || !new_invertible.has_value())
        return true;
    return old_invertible.value() != new_invertible.value();
}

static bool accumulated_visual_context_change_requires_repaint(CSS::PropertyID property_id, StyleValue const* old_value, StyleValue const* new_value, ComputedValues const* old_computed_values, ComputedValues const* new_computed_values)
{
    if (opacity_change_affects_paintable_visibility(property_id, old_value, new_value, old_computed_values, new_computed_values))
        return true;

    if (transform_change_requires_repaint(property_id, old_value, new_value, old_computed_values, new_computed_values))
        return true;

    switch (property_id) {
    case CSS::PropertyID::BackgroundAttachment:
    case CSS::PropertyID::Clip:
    case CSS::PropertyID::ClipPath:
    case CSS::PropertyID::MixBlendMode:
    case CSS::PropertyID::Perspective:
        return true;
    default:
        break;
    }

    return false;
}

// Whether this change only affects values carried by existing accumulated visual context nodes, so they can be
// patched in place. Presence flips (e.g. transform none <-> non-none) change the tree structure and which boxes
// establish abs/fixed positioning containing blocks, and must rebuild instead.
// NB: Must only include properties whose data update_accumulated_visual_context_values() recomputes.
static bool accumulated_visual_context_change_is_value_only(CSS::PropertyID property_id, StyleValue const* old_value, StyleValue const* new_value, ComputedValues const* old_computed_values, ComputedValues const* new_computed_values)
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
        return is_stacking_context_creating_value(property_id, old_value, old_computed_values)
            && is_stacking_context_creating_value(property_id, new_value, new_computed_values);
    default:
        return false;
    }
}

RequiredInvalidationAfterStyleChange compute_property_invalidation(CSS::PropertyID property_id, StyleValue const* old_value, StyleValue const* new_value, ComputedValues const* old_computed_values, ComputedValues const* new_computed_values)
{
    RequiredInvalidationAfterStyleChange invalidation;

    if (old_value == new_value)
        return invalidation;
    if (old_value && new_value && old_value->equals(*new_value))
        return invalidation;

    // NOTE: If the computed CSS display, position, content, or content-visibility property changes, we have to rebuild the entire layout tree.
    //       In the future, we should figure out ways to rebuild a smaller part of the tree.
    if (AK::first_is_one_of(property_id, CSS::PropertyID::Display, CSS::PropertyID::Position, CSS::PropertyID::Content, CSS::PropertyID::ContentVisibility)) {
        return RequiredInvalidationAfterStyleChange::full();
    }

    // NOTE: If the text-transform property changes, it may affect layout. Furthermore, since the
    //       Layout::TextNode caches the post-transform text, we have to update the layout tree.
    if (property_id == CSS::PropertyID::TextTransform) {
        invalidation.ensure_at_least(InvalidationLevel::RebuildLayoutTree);
        return invalidation;
    }

    // NOTE: If one of the overflow properties change, we rebuild the entire layout tree.
    //       This ensures that overflow propagation from root/body to viewport happens correctly.
    //       In the future, we can make this invalidation narrower.
    if (property_id == CSS::PropertyID::OverflowX || property_id == CSS::PropertyID::OverflowY) {
        return RequiredInvalidationAfterStyleChange::full();
    }

    if (AK::first_is_one_of(property_id, CSS::PropertyID::CounterReset, CSS::PropertyID::CounterSet, CSS::PropertyID::CounterIncrement)) {
        invalidation.ensure_at_least(InvalidationLevel::RebuildLayoutTree);
        return invalidation;
    }

    if (AK::first_is_one_of(property_id, CSS::PropertyID::ContainerName, CSS::PropertyID::ContainerType))
        invalidation.recompute_descendant_styles = true;

    // OPTIMIZATION: Special handling for CSS `visibility`:
    if (property_id == CSS::PropertyID::Visibility) {
        // We don't need to relayout if the visibility changes from visible to hidden or vice versa. Only collapse requires relayout.
        auto old_is_collapsed = old_computed_values ? old_computed_values->visibility() == Visibility::Collapse : old_value && old_value->to_keyword() == CSS::Keyword::Collapse;
        auto new_is_collapsed = new_computed_values ? new_computed_values->visibility() == Visibility::Collapse : new_value && new_value->to_keyword() == CSS::Keyword::Collapse;
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
            bool old_creates = is_stacking_context_creating_value(property_id, old_value, old_computed_values);
            bool new_creates = is_stacking_context_creating_value(property_id, new_value, new_computed_values);
            if (old_creates != new_creates) {
                invalidation.set_needs_stacking_context_tree_rebuild();
            }
        }
    }

    if (is_containing_block_establishing_value(property_id, old_value) != is_containing_block_establishing_value(property_id, new_value))
        invalidation.changes_containing_block_establishment = true;

    bool needs_repaint = true;
    if (CSS::property_affects_accumulated_visual_contexts(property_id)) {
        if (accumulated_visual_context_change_is_value_only(property_id, old_value, new_value, old_computed_values, new_computed_values))
            invalidation.ensure_at_least(AccumulatedVisualContextInvalidation::UpdateValues);
        else
            invalidation.ensure_at_least(AccumulatedVisualContextInvalidation::Rebuild);
        if (!accumulated_visual_context_change_requires_repaint(property_id, old_value, new_value, old_computed_values, new_computed_values)
            && !invalidation.needs_repaint()
            && !invalidation.recompute_descendant_styles)
            needs_repaint = false;
    }
    if (needs_repaint)
        invalidation.ensure_at_least(InvalidationLevel::Repaint);

    return invalidation;
}

}
