/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <LibGfx/Path.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Layout/SVGGeometryBox.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGPathElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGPathElement);

SVGPathElement::SVGPathElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, move(qualified_name))
{
}

Gfx::Path SVGPathElement::get_path(CSSPixelSize)
{
    auto computed_d = this->computed_values()->d();

    if (computed_d->is_keyword()) {
        VERIFY(computed_d->as_keyword().keyword() == CSS::Keyword::None);
        return {};
    }

    // NB: The reference box is not used for path() values and is only required to maintain compatibility with other
    //     basic shapes.
    return computed_d->as_basic_shape().basic_shape().get<CSS::Path>().to_path({ 0, 0, 0, 0 });
}

}
