/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

Environment::Environment(Environment* outer_environment, IsDeclarative is_declarative)
    : m_declarative(is_declarative == IsDeclarative::Yes)
    , m_outer_environment(outer_environment)
{
}

void Environment::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_outer_environment);
}

void Environment::set_permanently_screwed_by_eval()
{
    if (m_permanently_screwed_by_eval)
        return;
    m_permanently_screwed_by_eval = true;
    if (outer_environment())
        outer_environment()->set_permanently_screwed_by_eval();
}

}
