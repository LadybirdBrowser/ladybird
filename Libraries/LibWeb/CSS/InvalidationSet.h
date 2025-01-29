/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/HashTable.h>
#include <AK/Traits.h>
#include <LibWeb/CSS/PseudoClass.h>

namespace Web::CSS {

class InvalidationSet {
public:
    struct Property {
        enum class Type : u8 {
            InvalidateSelf,
            InvalidateWholeSubtree,
            Class,
            Id,
            TagName,
            Attribute,
            PseudoClass,
        };

        Type type;
        Variant<FlyString, PseudoClass, Empty> value { Empty {} };

        FlyString const& name() const { return value.get<FlyString>(); }

        bool operator==(Property const& other) const = default;
    };

    void include_all_from(InvalidationSet const& other);

    bool needs_invalidate_self() const { return m_needs_invalidate_self; }
    void set_needs_invalidate_self() { m_needs_invalidate_self = true; }

    bool needs_invalidate_whole_subtree() const { return m_needs_invalidate_whole_subtree; }
    void set_needs_invalidate_whole_subtree() { m_needs_invalidate_whole_subtree = true; }

    void set_needs_invalidate_class(FlyString const& name) { m_properties.set({ Property::Type::Class, name }); }
    void set_needs_invalidate_id(FlyString const& name) { m_properties.set({ Property::Type::Id, name }); }
    void set_needs_invalidate_tag_name(FlyString const& name) { m_properties.set({ Property::Type::TagName, name }); }
    void set_needs_invalidate_attribute(FlyString const& name) { m_properties.set({ Property::Type::Attribute, name }); }
    void set_needs_invalidate_pseudo_class(PseudoClass pseudo_class) { m_properties.set({ Property::Type::PseudoClass, pseudo_class }); }

    bool is_empty() const;
    void for_each_property(Function<IterationDecision(Property const&)> const& callback) const;

private:
    bool m_needs_invalidate_self { false };
    bool m_needs_invalidate_whole_subtree { false };
    HashTable<Property> m_properties;
};

}

namespace AK {

template<>
struct Traits<Web::CSS::InvalidationSet::Property> : DefaultTraits<String> {
    static unsigned hash(Web::CSS::InvalidationSet::Property const&);
    static bool equals(Web::CSS::InvalidationSet::Property const& a, Web::CSS::InvalidationSet::Property const& b) { return a == b; }
};

template<>
struct Formatter<Web::CSS::InvalidationSet::Property> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, Web::CSS::InvalidationSet::Property const&);
};

template<>
struct Formatter<Web::CSS::InvalidationSet> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, Web::CSS::InvalidationSet const&);
};

}
