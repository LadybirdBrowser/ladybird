/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObjectSerializer.h>
#include <LibWeb/ARIA/AriaData.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/ARIA/StateAndProperties.h>

namespace Web::ARIA {

enum class NameFromSource {
    Author,
    Content,
    AuthorContent,
    Prohibited
};

// https://www.w3.org/TR/wai-aria-1.2/#roletype
// The base role from which all other roles inherit.
class RoleType {
public:
    static ErrorOr<NonnullOwnPtr<RoleType>> build_role_object(Role, bool, AriaData const&);

    virtual ~RoleType() = default;
    // https://www.w3.org/TR/wai-aria-1.2/#supportedState
    virtual HashTable<StateAndProperties> const& supported_states() const;
    virtual HashTable<StateAndProperties> const& supported_properties() const;
    // https://www.w3.org/TR/wai-aria-1.2/#requiredState
    virtual HashTable<StateAndProperties> const& required_states() const;
    virtual HashTable<StateAndProperties> const& required_properties() const;
    // https://www.w3.org/TR/wai-aria-1.2/#prohibitedattributes
    virtual HashTable<StateAndProperties> const& prohibited_properties() const;
    virtual HashTable<StateAndProperties> const& prohibited_states() const;
    // https://www.w3.org/TR/wai-aria-1.2/#scope
    virtual HashTable<Role> const& required_context_roles() const;
    // https://www.w3.org/TR/wai-aria-1.2/#mustContain
    virtual HashTable<Role> const& required_owned_elements() const;
    // https://www.w3.org/TR/wai-aria-1.2/#namecalculation
    virtual NameFromSource name_from_source() const = 0;
    virtual bool accessible_name_required() const { return false; }
    // https://www.w3.org/TR/wai-aria-1.2/#childrenArePresentational
    virtual bool children_are_presentational() const { return false; }
    // https://www.w3.org/TR/wai-aria-1.2/#implictValueForRole
    using DefaultValueType = Variant<Empty, f64, AriaOrientation, AriaLive, bool, AriaHasPopup>;
    virtual DefaultValueType default_value_for_property_or_state(StateAndProperties) const { return {}; }
    ErrorOr<void> serialize_as_json(JsonObjectSerializer<StringBuilder>& object) const;

protected:
    RoleType(AriaData const&);
    RoleType() { }

private:
    AriaData m_data;
};

}
