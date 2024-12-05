/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/ARIA/AriaData.h>
#include <LibWeb/ARIA/AttributeNames.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::ARIA {

class ARIAMixin {
public:
    virtual ~ARIAMixin() = default;

#define __ENUMERATE_ARIA_ATTRIBUTE(name, attribute) \
    virtual Optional<String> name() const = 0;      \
    virtual WebIDL::ExceptionOr<void> set_##name(Optional<String> const&) = 0;
    ENUMERATE_ARIA_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

    // https://www.w3.org/TR/html-aria/#docconformance
    virtual Optional<Role> default_role() const { return {}; }

    Optional<Role> role_or_default() const;

    // https://www.w3.org/TR/wai-aria-1.2/#tree_exclusion
    virtual bool exclude_from_accessibility_tree() const = 0;

    // https://www.w3.org/TR/wai-aria-1.2/#tree_inclusion
    virtual bool include_in_accessibility_tree() const = 0;

    bool has_global_aria_attribute() const;

    // https://www.w3.org/TR/wai-aria-1.2/#valuetype_idref
    Optional<String> parse_id_reference(Optional<String> const&) const;

    // https://www.w3.org/TR/wai-aria-1.2/#valuetype_idref_list
    Vector<String> parse_id_reference_list(Optional<String> const&) const;

protected:
    ARIAMixin() = default;

    virtual bool id_reference_exists(String const&) const = 0;
};

}
