/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ContainerQuery.h"
#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/BooleanExpression.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

NonnullRefPtr<ContainerQuery> ContainerQuery::create(NonnullOwnPtr<BooleanExpression>&& condition)
{
    return adopt_ref(*new ContainerQuery(move(condition)));
}

ContainerQuery::ContainerQuery(NonnullOwnPtr<BooleanExpression>&& condition)
    : m_condition(move(condition))
    , m_matches(m_condition->evaluate_to_boolean({}))
{
    m_condition->collect_container_query_feature_requirements(m_feature_requirements);
}

static bool container_satisfies_requirements(DOM::Element const& element, ContainerQueryFeatureRequirements const& requirements)
{
    auto style = element.computed_properties();
    if (!style)
        return false;

    auto container_type = style->container_type();

    if (requirements.requires_size_container && !container_type.is_size_container)
        return false;

    if (requirements.requires_inline_size_container && !(container_type.is_size_container || container_type.is_inline_size_container))
        return false;

    if (requirements.requires_block_size_container && !container_type.is_size_container)
        return false;

    if (requirements.requires_scroll_state_container && !container_type.is_scroll_state_container)
        return false;

    return true;
}

// https://drafts.csswg.org/css-conditional-5/#container-rule
MatchResult ContainerQuery::evaluate(DOM::AbstractElement const& element, Optional<FlyString> const& container_name) const
{
    // If the <container-query> contains unknown or unsupported container features, no query container will be selected
    // for that <container-condition>.
    if (m_feature_requirements.has_unknown_or_unsupported_feature)
        return MatchResult::Unknown;

    // For each element, the query container to be queried is selected from among the element’s ancestor query
    // containers that are established as a valid query container for all the container features in the
    // <container-query>.
    for (auto const* container = element.element().flat_tree_parent_element(); container; container = container->flat_tree_parent_element()) {
        // The <container-name> filters the set of query containers considered to just those with a matching query
        // container name.
        if (!container_name_matches(*container, container_name))
            continue;

        if (!container_satisfies_requirements(*container, m_feature_requirements))
            continue;

        // Once an eligible query container has been selected for an element, each container feature in the
        // <container-query> is evaluated against that query container.
        return m_condition->evaluate({
            .document = &element.document(),
            .query_container = container,
        });
    }

    // If no ancestor is an eligible query container, then the container query is unknown for that element.
    return MatchResult::Unknown;
}

String ContainerQuery::to_string() const
{
    return m_condition->to_string();
}

void ContainerQuery::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("Container query: (matches = {})\n", m_matches);
    m_condition->dump(builder, indent_levels + 1);
}

bool container_name_matches(DOM::Element const& element, Optional<FlyString> const& container_name)
{
    if (!container_name.has_value())
        return true;

    if (auto style = element.computed_properties())
        return style->container_name().contains_slow(*container_name);

    return false;
}

}
