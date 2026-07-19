/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

struct RandomCachingKey {
    Utf16FlyString name;
    Optional<Web::UniqueNodeID> element_id;
};

class RandomValueSharingStyleValue : public StyleValueWithDefaultOperators<RandomValueSharingStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RandomValueSharingStyleValue const> create_fixed(NonnullRefPtr<StyleValue const> const& fixed_value)
    {
        return adopt_ref(*new (nothrow) RandomValueSharingStyleValue(fixed_value, false, {}, false));
    }

    static ValueComparingNonnullRefPtr<RandomValueSharingStyleValue const> create_auto(Utf16FlyString name, bool element_shared)
    {
        return adopt_ref(*new (nothrow) RandomValueSharingStyleValue({}, true, move(name), element_shared));
    }

    static ValueComparingNonnullRefPtr<RandomValueSharingStyleValue const> create_dashed_ident(Utf16FlyString name, bool element_shared)
    {
        return adopt_ref(*new (nothrow) RandomValueSharingStyleValue({}, false, move(name), element_shared));
    }

    virtual ~RandomValueSharingStyleValue() override = default;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    double random_base_value() const;

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(RandomValueSharingStyleValue const& other) const
    {
        return fixed_value() == other.fixed_value()
            && is_auto() == other.is_auto()
            && name() == other.name()
            && element_shared() == other.element_shared();
    }

private:
    explicit RandomValueSharingStyleValue(RefPtr<StyleValue const> fixed_value, bool is_auto, Optional<Utf16FlyString> name, bool element_shared)
        : StyleValueWithDefaultOperators(Type::RandomValueSharing, make_random_value_sharing_data(fixed_value, is_auto, name, element_shared))
    {
    }

    static StyleValueFFI::StyleValueData* make_random_value_sharing_data(RefPtr<StyleValue const> const& fixed_value, bool is_auto, Optional<Utf16FlyString> const& name, bool element_shared)
    {
        // The Rust allocation takes ownership of one strong reference to the fixed value.
        if (fixed_value)
            fixed_value->ref();
        return StyleValueFFI::rust_style_value_create_random_value_sharing(fixed_value.ptr(), is_auto, name.has_value(), name.has_value() ? name->to_raw_leaked() : 0, element_shared);
    }

    ValueComparingRefPtr<StyleValue const> fixed_value() const { return static_cast<StyleValue const*>(m_value->random_value_sharing.fixed_value.pointer); }
    bool is_auto() const { return m_value->random_value_sharing.is_auto; }
    Optional<Utf16FlyString> name() const
    {
        if (!m_value->random_value_sharing.has_name)
            return {};
        return Utf16FlyString::from_raw(m_value->random_value_sharing.name.raw);
    }
    bool element_shared() const { return m_value->random_value_sharing.element_shared; }
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
