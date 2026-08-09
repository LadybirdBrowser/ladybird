/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/EmbeddedContentInvalidator.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_embedded_content_geometry_change(DOM::Element& element)
{
    // FIXME: This should only invalidate the layout, not the style.
    element.record_style_environment_change();
}

}
