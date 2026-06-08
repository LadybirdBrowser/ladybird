/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

class WEB_API PropertyNameAndID {
public:
    static Optional<PropertyNameAndID> from_name(Utf16FlyString name)
    {
        if (is_a_custom_property_name_string(name))
            return PropertyNameAndID(move(name), PropertyID::Custom);

        if (!name.is_ascii())
            return {};

        auto name_string = name.to_utf16_string();
        if (auto property_id = property_id_from_string(name_string.ascii_view()); property_id.has_value())
            return PropertyNameAndID(Utf16FlyString::from_utf8(string_from_property_id(property_id.value())), property_id.value());

        return {};
    }

    static PropertyNameAndID from_id(PropertyID property_id)
    {
        VERIFY(property_id != PropertyID::Custom);
        return PropertyNameAndID({}, property_id);
    }

    bool is_custom_property() const { return m_property_id == PropertyID::Custom; }
    PropertyID id() const { return m_property_id; }

    Utf16FlyString const& name() const
    {
        if (!m_name.has_value())
            m_name = Utf16FlyString::from_utf8(string_from_property_id(m_property_id));
        return m_name.value();
    }

    String to_string() const
    {
        return serialize_an_identifier(name().to_utf16_string().to_utf8_but_should_be_ported_to_utf16());
    }

private:
    PropertyNameAndID(Optional<Utf16FlyString> name, PropertyID id)
        : m_name(move(name))
        , m_property_id(id)
    {
    }

    mutable Optional<Utf16FlyString> m_name;
    PropertyID m_property_id;
};

}
