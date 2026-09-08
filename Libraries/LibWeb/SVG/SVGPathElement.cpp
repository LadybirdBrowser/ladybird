/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <LibGfx/Path.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGPathElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGPathElement);

SVGPathElement::SVGPathElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, move(qualified_name))
{
}

Gfx::Path SVGPathElement::get_path(CSSPixelSize, CSS::ComputedValues const& computed_values)
{
    auto computed_d = computed_values.d();

    if (computed_d->is_keyword()) {
        VERIFY(computed_d->as_keyword().keyword() == CSS::Keyword::None);
        return {};
    }

    VERIFY(computed_d->is_basic_shape());
    auto const& shape = computed_d->rust_style_value_data()->basic_shape;
    VERIFY(shape.kind == 6);
    auto* native_path = static_cast<Gfx::Path*>(CSS::StyleValueFFI::rust_css_path_to_gfx_path(&shape.path));
    auto path = move(*native_path);
    delete native_path;
    static_assert(to_underlying(Gfx::WindingRule::Nonzero) == 0);
    static_assert(to_underlying(Gfx::WindingRule::EvenOdd) == 1);
    path.set_fill_type(static_cast<Gfx::WindingRule>(shape.fill_rule));
    return path;
}

}
