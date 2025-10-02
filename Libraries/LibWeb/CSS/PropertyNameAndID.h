/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/GenericShorthands.h>
#include <AK/Optional.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

class WEB_API PropertyNameAndID {
public:
    static Optional<PropertyNameAndID> from_name(FlyString name)
    {
        if (is_a_custom_property_name_string(name))
            return PropertyNameAndID(move(name), PropertyID::Custom);

        if (auto property_id = property_id_from_string(name); property_id.has_value())
            return PropertyNameAndID(string_from_property_id(property_id.value()), property_id.value());

        return {};
    }

    static PropertyNameAndID from_id(PropertyID property_id)
    {
        VERIFY(property_id != PropertyID::Custom);
        return PropertyNameAndID({}, property_id);
    }

    bool is_custom_property() const { return m_property_id == PropertyID::Custom; }
    PropertyID id() const { return m_property_id; }

    FlyString const& name() const
    {
        if (!m_name.has_value())
            m_name = string_from_property_id(m_property_id);
        return m_name.value();
    }

    String to_string() const
    {
        return serialize_an_identifier(name());
    }

private:
    PropertyNameAndID(Optional<FlyString> name, PropertyID id)
        : m_name(move(name))
        , m_property_id(id)
    {
    }

    mutable Optional<FlyString> m_name;
    PropertyID m_property_id;
};

}
