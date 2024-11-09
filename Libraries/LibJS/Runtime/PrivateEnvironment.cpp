/*
 * Copyright (c) 2021, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/PrivateEnvironment.h>

namespace JS {

JS_DEFINE_ALLOCATOR(PrivateEnvironment);

PrivateEnvironment::PrivateEnvironment(PrivateEnvironment* parent)
    : m_outer_environment(parent)
    , m_unique_id(s_next_id++)
{
    // FIXME: We might want to delay getting the next unique id until required.
    VERIFY(s_next_id != 0);
}

// Note: we start at one such that 0 can be invalid / default initialized.
u64 PrivateEnvironment::s_next_id = 1u;

PrivateName PrivateEnvironment::resolve_private_identifier(DeprecatedFlyString const& identifier) const
{
    auto name_or_end = find_private_name(identifier);

    if (!name_or_end.is_end())
        return *name_or_end;

    // Note: This verify ensures that we must either have a private name with a matching description
    //       or have an outer environment. Combined this means that we assert that we always return a PrivateName.
    VERIFY(m_outer_environment);
    return m_outer_environment->resolve_private_identifier(identifier);
}

void PrivateEnvironment::add_private_name(DeprecatedFlyString description)
{
    if (!find_private_name(description).is_end())
        return;

    m_private_names.empend(m_unique_id, move(description));
}

bool PrivateName::operator==(PrivateName const& rhs) const
{
    return unique_id == rhs.unique_id && description == rhs.description;
}

void PrivateEnvironment::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_outer_environment);
}

}
