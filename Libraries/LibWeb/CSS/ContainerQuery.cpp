/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ContainerQuery.h"
#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/BooleanExpression.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

NonnullRefPtr<ContainerQuery> ContainerQuery::create(NonnullOwnPtr<BooleanExpression>&& condition)
{
    return adopt_ref(*new ContainerQuery(move(condition)));
}

ContainerQuery::ContainerQuery(NonnullOwnPtr<BooleanExpression>&& condition)
    : m_condition(move(condition))
    , m_matches(m_condition->evaluate_to_boolean(nullptr))
{
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

}
