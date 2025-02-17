/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Pattern/Pattern.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#url-pattern-has-regexp-groups
bool Pattern::has_regexp_groups() const
{
    // 1. If urlPattern’s protocol component has regexp groups is true, then return true.
    if (m_protocol_component.has_regexp_groups)
        return true;

    // 2. If urlPattern’s username component has regexp groups is true, then return true.
    if (m_username_component.has_regexp_groups)
        return true;

    // 3. If urlPattern’s password component has regexp groups is true, then return true.
    if (m_password_component.has_regexp_groups)
        return true;

    // 4. If urlPattern’s hostname component has regexp groups is true, then return true.
    if (m_hostname_component.has_regexp_groups)
        return true;

    // 5. If urlPattern’s port component has regexp groups is true, then return true.
    if (m_port_component.has_regexp_groups)
        return true;

    // 6. If urlPattern’s pathname component has regexp groups is true, then return true.
    if (m_pathname_component.has_regexp_groups)
        return true;

    // 7. If urlPattern’s search component has regexp groups is true, then return true.
    if (m_search_component.has_regexp_groups)
        return true;

    // 8. If urlPattern’s hash component has regexp groups is true, then return true.
    if (m_hash_component.has_regexp_groups)
        return true;

    // 9. Return false.
    return false;
}

}
