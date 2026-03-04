/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/DescriptorID.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

class DescriptorNameAndID {
public:
    static Optional<DescriptorNameAndID> from_name(AtRuleID at_rule_id, FlyString name)
    {
        if (is_a_custom_property_name_string(name))
            return DescriptorNameAndID(move(name), DescriptorID::Custom);

        if (auto descriptor_id = descriptor_id_from_string(at_rule_id, name); descriptor_id.has_value()) {
            // NB: We use the serialized ID as the name here instead of the input name to ensure that legacy alias
            //     mapping is reflected
            return DescriptorNameAndID(CSS::to_string(descriptor_id.value()), descriptor_id.value());
        }

        return {};
    }

    static DescriptorNameAndID from_id(DescriptorID descriptor_id)
    {
        VERIFY(descriptor_id != DescriptorID::Custom);
        return DescriptorNameAndID(CSS::to_string(descriptor_id), descriptor_id);
    }

    DescriptorID id() const { return m_id; }

    FlyString const& name() const
    {
        return m_name;
    }

    bool operator==(DescriptorNameAndID const& other) const
    {
        return m_id == other.m_id && m_name == other.m_name;
    }

private:
    DescriptorNameAndID(FlyString name, DescriptorID id)
        : m_name(move(name))
        , m_id(id)
    {
    }

    FlyString m_name;
    DescriptorID m_id;
};

}

namespace AK {

template<>
struct Traits<Web::CSS::DescriptorNameAndID> : public DefaultTraits<Web::CSS::DescriptorNameAndID> {
    static unsigned hash(Web::CSS::DescriptorNameAndID const& descriptor_name_and_id)
    {
        return pair_int_hash(to_underlying(descriptor_name_and_id.id()), descriptor_name_and_id.name().hash());
    }
};

}
