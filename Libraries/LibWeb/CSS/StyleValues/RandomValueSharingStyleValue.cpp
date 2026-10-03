/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RandomValueSharingStyleValue.h"
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> RandomValueSharingStyleValue::absolutized(ComputationContext const& computation_context) const
{
    // https://drafts.csswg.org/css-values-5/#random-caching
    // Each instance of a random function in styles has an associated random base value.
    // If the random function’s <random-value-sharing> is fixed <number>, the random base value is that number.
    if (fixed_value()) {
        auto const& absolutized_fixed_value = fixed_value()->absolutized(computation_context);

        if (fixed_value() == absolutized_fixed_value)
            return *this;

        return RandomValueSharingStyleValue::create_fixed(absolutized_fixed_value);
    }

    // Otherwise, the random base value is a pseudo-random real number in the range `[0, 1)` (greater than or equal to 0
    // and less than 1), generated from a uniform distribution, and influenced by the function’s random caching key.

    // A random caching key is a tuple of:
    // 1. A string name: the value of the <dashed-ident>, if specified in <random-value-sharing>; or else a string
    //    of the form "PROPERTY N", where PROPERTY is the name of the property the random function is used in
    //    (before shorthand expansion, if relevant), and N is the index of the random function among other random
    //    functions in the same property value.
    // 2. An element ID identifying the element the style is being applied to, or null if element-shared is
    //    specified in <random-value-sharing>.
    // 3. A document ID identifying the Document the styles are from.
    // NB: The style engine keeps the base values, one engine per document. A key names the element by its style
    //     node, and a pseudo-element's by its element's. A <dashed-ident> is shared by every element, as the
    //     style computation shares it.
    auto name = this->name().value();
    auto const& element = computation_context.abstract_element->element();
    auto& style_engine = const_cast<StyleEngine&>(element.document().style_computer().style_engine());
    // The engine keeps the base values, so asking for one is the computation's own read of the render state.
    Layout::ForcedReadScope read { element.document(), false };
    auto random_base_value = style_engine.ensure_random_base_value(read, element.style_node_id(), name.view(), element_shared() || !is_auto());

    return RandomValueSharingStyleValue::create_fixed(NumberStyleValue::create(random_base_value));
}

double RandomValueSharingStyleValue::random_base_value() const
{
    return number_from_style_value(*fixed_value(), {});
}

}
