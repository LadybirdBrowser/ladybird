/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleProperties.h>

namespace Web::CSS {

RequiredInvalidationAfterStyleChange compute_property_invalidation(CSS::PropertyID property_id, RefPtr<CSSStyleValue const> const& old_value, RefPtr<CSSStyleValue const> const& new_value)
{
    RequiredInvalidationAfterStyleChange invalidation;

    bool const property_value_changed = (!old_value || !new_value) || *old_value != *new_value;
    if (!property_value_changed)
        return invalidation;

    // NOTE: If the computed CSS display, content, or content-visibility property changes, we have to rebuild the entire layout tree.
    //       In the future, we should figure out ways to rebuild a smaller part of the tree.
    if (AK::first_is_one_of(property_id, CSS::PropertyID::Display, CSS::PropertyID::Content, CSS::PropertyID::ContentVisibility)) {
        return RequiredInvalidationAfterStyleChange::full();
    }

    // NOTE: If one of the overflow properties change, we rebuild the entire layout tree.
    //       This ensures that overflow propagation from root/body to viewport happens correctly.
    //       In the future, we can make this invalidation narrower.
    if (property_id == CSS::PropertyID::OverflowX || property_id == CSS::PropertyID::OverflowY) {
        return RequiredInvalidationAfterStyleChange::full();
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

    if (property_id == CSS::PropertyID::Opacity && old_value && new_value) {
        // OPTIMIZATION: An element creates a stacking context when its opacity changes from 1 to less than 1
        //               and stops to create one when opacity returns to 1. So stacking context tree rebuild is
        //               not required for opacity changes within the range below 1.
        auto old_value_opacity = CSS::StyleProperties::resolve_opacity_value(*old_value);
        auto new_value_opacity = CSS::StyleProperties::resolve_opacity_value(*new_value);
        if (old_value_opacity != new_value_opacity && (old_value_opacity == 1 || new_value_opacity == 1)) {
            invalidation.rebuild_stacking_context_tree = true;
        }
    } else if (CSS::property_affects_stacking_context(property_id)) {
        invalidation.rebuild_stacking_context_tree = true;
    }
    invalidation.repaint = true;

    return invalidation;
}

}
