/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RandomValueSharingStyleValue.h"
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> RandomValueSharingStyleValue::absolutized(ComputationContext const& computation_context) const
{
    // https://drafts.csswg.org/css-values-5/#random-caching
    // Each instance of a random function in styles has an associated random base value.
    // If the random function’s <random-value-sharing> is fixed <number>, the random base value is that number.
    if (m_fixed_value) {
        auto const& absolutized_fixed_value = m_fixed_value->absolutized(computation_context);

        if (m_fixed_value == absolutized_fixed_value)
            return *this;

        return RandomValueSharingStyleValue::create_fixed(absolutized_fixed_value);
    }

    // Otherwise, the random base value is a pseudo-random real number in the range `[0, 1)` (greater than or equal to 0
    // and less than 1), generated from a uniform distribution, and influenced by the function’s random caching key.

    // A random caching key is a tuple of:
    RandomCachingKey random_caching_key {
        // 1. A string name: the value of the <dashed-ident>, if specified in <random-value-sharing>; or else a string
        //    of the form "PROPERTY N", where PROPERTY is the name of the property the random function is used in
        //    (before shorthand expansion, if relevant), and N is the index of the random function among other random
        //    functions in the same property value.
        .name = m_name.value(),

        // 2. An element ID identifying the element the style is being applied to, or null if element-shared is
        //    specified in <random-value-sharing>.
        // FIXME: Use the pseudo element's unique_id() when that's accessible
        .element_id = m_element_shared ? Optional<UniqueNodeID> { OptionalNone {} } : Optional<UniqueNodeID> { computation_context.abstract_element->element().unique_id() },

        // 3. A document ID identifying the Document the styles are from.
        // NB: This is implicit since the cache is stored on the document or the element (which is a child of the document).
    };

    auto random_base_value = const_cast<DOM::Element&>(computation_context.abstract_element->element()).ensure_css_random_base_value(random_caching_key);

    return RandomValueSharingStyleValue::create_fixed(NumberStyleValue::create(random_base_value));
}

double RandomValueSharingStyleValue::random_base_value() const
{
    VERIFY(m_fixed_value);
    VERIFY(m_fixed_value->is_number() || (m_fixed_value->is_calculated() && m_fixed_value->as_calculated().resolves_to_number()));

    if (m_fixed_value->is_number())
        return m_fixed_value->as_number().number();

    if (m_fixed_value->is_calculated())
        return m_fixed_value->as_calculated().resolve_number({}).value();

    VERIFY_NOT_REACHED();
}

String RandomValueSharingStyleValue::to_string(SerializationMode serialization_mode) const
{
    if (m_fixed_value)
        return MUST(String::formatted("fixed {}", m_fixed_value->to_string(serialization_mode)));

    StringBuilder builder;

    if (!m_is_auto)
        builder.appendff("{}", m_name.value());

    if (m_element_shared) {
        if (!builder.is_empty())
            builder.append(' ');
        builder.append("element-shared"sv);
    }

    return builder.to_string_without_validation();
}

}
