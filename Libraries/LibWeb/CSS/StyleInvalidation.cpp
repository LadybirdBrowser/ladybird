/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>

namespace Web::CSS {

static bool is_stacking_context_creating_value(CSS::PropertyID property_id, RefPtr<StyleValue const> const& value)
{
    if (!value)
        return false;

    switch (property_id) {
    case CSS::PropertyID::Opacity:
        return !value->is_number() || value->as_number().number() != 1;
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
        return value->is_filter_value_list();
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

RequiredInvalidationAfterStyleChange compute_property_invalidation(CSS::PropertyID property_id, RefPtr<StyleValue const> const& old_value, RefPtr<StyleValue const> const& new_value)
{
    RequiredInvalidationAfterStyleChange invalidation;

    bool const property_value_changed = (old_value || new_value) && ((!old_value || !new_value) || *old_value != *new_value);
    if (!property_value_changed)
        return invalidation;

    // NOTE: If the computed CSS display, position, content, or content-visibility property changes, we have to rebuild the entire layout tree.
    //       In the future, we should figure out ways to rebuild a smaller part of the tree.
    if (AK::first_is_one_of(property_id, CSS::PropertyID::Display, CSS::PropertyID::Position, CSS::PropertyID::Content, CSS::PropertyID::ContentVisibility)) {
        return RequiredInvalidationAfterStyleChange::full();
    }

    // NOTE: If the text-transform property changes, it may affect layout. Furthermore, since the
    //       Layout::TextNode caches the post-transform text, we have to update the layout tree.
    if (property_id == CSS::PropertyID::TextTransform) {
        invalidation.rebuild_layout_tree = true;
        invalidation.relayout = true;
        invalidation.repaint = true;
        return invalidation;
    }

    // NOTE: If one of the overflow properties change, we rebuild the entire layout tree.
    //       This ensures that overflow propagation from root/body to viewport happens correctly.
    //       In the future, we can make this invalidation narrower.
    if (property_id == CSS::PropertyID::OverflowX || property_id == CSS::PropertyID::OverflowY) {
        return RequiredInvalidationAfterStyleChange::full();
    }

    if (AK::first_is_one_of(property_id, CSS::PropertyID::CounterReset, CSS::PropertyID::CounterSet, CSS::PropertyID::CounterIncrement)) {
        invalidation.rebuild_layout_tree = property_value_changed;
        return invalidation;
    }

    // OPTIMIZATION: Special handling for CSS `visibility`:
    if (property_id == CSS::PropertyID::Visibility) {
        // We don't need to relayout if the visibility changes from visible to hidden or vice versa. Only collapse requires relayout.
        if ((old_value && old_value->to_keyword() == CSS::Keyword::Collapse) != (new_value && new_value->to_keyword() == CSS::Keyword::Collapse))
            invalidation.relayout = true;
        // Of course, we still have to repaint on any visibility change.
        invalidation.repaint = true;
    } else if (CSS::property_affects_layout(property_id)) {
        invalidation.relayout = true;
    }

    if (CSS::property_affects_stacking_context(property_id)) {
        // OPTIMIZATION: Only rebuild stacking context tree when property crosses from a neutral value (doesn't create
        //               stacking context) to a creating value or vice versa.
        bool old_creates = is_stacking_context_creating_value(property_id, old_value);
        bool new_creates = is_stacking_context_creating_value(property_id, new_value);
        if (old_creates != new_creates) {
            invalidation.rebuild_stacking_context_tree = true;
        }
    }
    invalidation.repaint = true;

    // Transform, perspective, clip, clip-path, and effects properties require rebuilding AccumulatedVisualContext tree.
    if (AK::first_is_one_of(property_id,
            CSS::PropertyID::Transform,
            CSS::PropertyID::Rotate,
            CSS::PropertyID::Scale,
            CSS::PropertyID::Translate,
            CSS::PropertyID::Perspective,
            CSS::PropertyID::TransformOrigin,
            CSS::PropertyID::PerspectiveOrigin,
            CSS::PropertyID::Clip,
            CSS::PropertyID::ClipPath,
            CSS::PropertyID::Opacity,
            CSS::PropertyID::MixBlendMode,
            CSS::PropertyID::Filter)) {
        invalidation.rebuild_accumulated_visual_contexts = true;
    }

    return invalidation;
}

}
