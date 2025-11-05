/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

struct RandomCachingKey {
    FlyString name;
    Optional<Web::UniqueNodeID> element_id;
};

class RandomValueSharingStyleValue : public StyleValueWithDefaultOperators<RandomValueSharingStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RandomValueSharingStyleValue const> create_fixed(NonnullRefPtr<StyleValue const> const& fixed_value)
    {
        return adopt_ref(*new (nothrow) RandomValueSharingStyleValue(fixed_value, false, {}, false));
    }

    static ValueComparingNonnullRefPtr<RandomValueSharingStyleValue const> create_auto(FlyString name, bool element_shared)
    {
        return adopt_ref(*new (nothrow) RandomValueSharingStyleValue({}, true, move(name), element_shared));
    }

    static ValueComparingNonnullRefPtr<RandomValueSharingStyleValue const> create_dashed_ident(FlyString name, bool element_shared)
    {
        return adopt_ref(*new (nothrow) RandomValueSharingStyleValue({}, false, move(name), element_shared));
    }

    virtual ~RandomValueSharingStyleValue() override = default;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    double random_base_value() const;

    virtual String to_string(SerializationMode serialization_mode) const override;

    bool properties_equal(RandomValueSharingStyleValue const& other) const
    {
        return m_fixed_value == other.m_fixed_value
            && m_is_auto == other.m_is_auto
            && m_name == other.m_name
            && m_element_shared == other.m_element_shared;
    }

private:
    explicit RandomValueSharingStyleValue(RefPtr<StyleValue const> fixed_value, bool is_auto, Optional<FlyString> name, bool element_shared)
        : StyleValueWithDefaultOperators(Type::RandomValueSharing)
        , m_fixed_value(move(fixed_value))
        , m_is_auto(is_auto)
        , m_name(move(name))
        , m_element_shared(element_shared)
    {
    }

    ValueComparingRefPtr<StyleValue const> m_fixed_value;
    bool m_is_auto;
    Optional<FlyString> m_name;
    bool m_element_shared;
};

}

namespace AK {

template<>
struct Traits<Web::CSS::RandomCachingKey> : public DefaultTraits<Web::CSS::RandomCachingKey> {
    static unsigned hash(Web::CSS::RandomCachingKey const& key)
    {
        if (!key.element_id.has_value())
            return key.name.hash();

        return pair_int_hash(key.name.hash(), Traits<i64>::hash(key.element_id->value()));
    }

    static bool equals(Web::CSS::RandomCachingKey const& a, Web::CSS::RandomCachingKey const& b)
    {
        return a.element_id == b.element_id && a.name == b.name;
    }
};

}
