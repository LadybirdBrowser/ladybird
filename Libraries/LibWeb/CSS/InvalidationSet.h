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
#include <AK/Variant.h>
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
    void set_needs_invalidate_self()
    {
        m_needs_invalidate_self = true;
        m_hash = {};
    }

    bool needs_invalidate_whole_subtree() const { return m_needs_invalidate_whole_subtree; }
    void set_needs_invalidate_whole_subtree()
    {
        m_needs_invalidate_whole_subtree = true;
        m_hash = {};
    }

    bool operator==(InvalidationSet const& other) const;

    void set_needs_invalidate_class(FlyString const& name)
    {
        add_property({ Property::Type::Class, name });
    }
    void set_needs_invalidate_id(FlyString const& name)
    {
        add_property({ Property::Type::Id, name });
    }
    void set_needs_invalidate_tag_name(FlyString const& name)
    {
        add_property({ Property::Type::TagName, name });
    }
    void set_needs_invalidate_attribute(FlyString const& name)
    {
        add_property({ Property::Type::Attribute, name });
    }
    void set_needs_invalidate_pseudo_class(PseudoClass pseudo_class)
    {
        add_property({ Property::Type::PseudoClass, pseudo_class });
    }

    bool is_empty() const;
    bool has_properties() const { return !m_properties.is_empty(); }
    void for_each_property(Function<IterationDecision(Property const&)> const& callback) const;

    u32 hash() const;

private:
    class PropertySet {
    public:
        PropertySet();
        PropertySet(PropertySet const&);
        PropertySet(PropertySet&&);
        ~PropertySet();

        PropertySet& operator=(PropertySet const&);
        PropertySet& operator=(PropertySet&&);

        bool is_empty() const;
        size_t size() const;
        bool contains(Property const&) const;
        bool set(Property);
        bool include_all_from(PropertySet const&);
        bool operator==(PropertySet const&) const;
        void accumulate_hash(u32& property_hash_sum, u32& property_hash_xor) const;

        IterationDecision for_each(Function<IterationDecision(Property const&)> const&) const;

    private:
        enum class StorageType : u8 {
            Empty,
            Single,
            HashTable,
        };

        void destroy_storage();
        void copy_from(PropertySet const&);
        void move_from(PropertySet&&);

        StorageType m_storage_type { StorageType::Empty };
        union {
            Empty m_empty;
            Property m_property;
            HashTable<Property> m_properties;
        };
    };

    void add_property(Property);

    bool m_needs_invalidate_self { false };
    bool m_needs_invalidate_whole_subtree { false };
    PropertySet m_properties;
    mutable Optional<u32> m_hash;
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
