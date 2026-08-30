/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>

namespace Web::CSS {

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

static bool clip_path_value_is_a_visual_context_frame(StyleValue const* value)
{
    return value && value->is_basic_shape();
}

static bool clip_value_is_a_visual_context_frame(StyleValue const* value)
{
    return value && value->is_rect();
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
        } else if (property_id == PropertyID::ClipPath) {
            requires_repaint = !clip_path_value_is_a_visual_context_frame(old_value) || !clip_path_value_is_a_visual_context_frame(new_value);
        } else if (property_id == PropertyID::Clip) {
            requires_repaint = !clip_value_is_a_visual_context_frame(old_value) || !clip_value_is_a_visual_context_frame(new_value);
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
