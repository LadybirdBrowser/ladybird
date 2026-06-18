/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/InvalidationSet.h>

namespace Web::CSS {

InvalidationSet::PropertySet::PropertySet()
    : m_empty()
{
}

InvalidationSet::PropertySet::PropertySet(PropertySet const& other)
    : m_empty()
{
    copy_from(other);
}

InvalidationSet::PropertySet::PropertySet(PropertySet&& other)
    : m_empty()
{
    move_from(move(other));
}

InvalidationSet::PropertySet::~PropertySet()
{
    destroy_storage();
}

InvalidationSet::PropertySet& InvalidationSet::PropertySet::operator=(PropertySet const& other)
{
    if (this != &other) {
        destroy_storage();
        copy_from(other);
    }
    return *this;
}

InvalidationSet::PropertySet& InvalidationSet::PropertySet::operator=(PropertySet&& other)
{
    if (this != &other) {
        destroy_storage();
        move_from(move(other));
    }
    return *this;
}

void InvalidationSet::PropertySet::destroy_storage()
{
    if (m_storage_type == StorageType::Single) {
        m_property.~Property();
        return;
    }
    if (m_storage_type == StorageType::HashTable)
        m_properties.~HashTable<Property>();
}

void InvalidationSet::PropertySet::copy_from(PropertySet const& other)
{
    m_storage_type = other.m_storage_type;
    if (m_storage_type == StorageType::Empty) {
        new (&m_empty) Empty();
        return;
    }
    if (m_storage_type == StorageType::Single) {
        new (&m_property) Property(other.m_property);
        return;
    }
    new (&m_properties) HashTable<Property>(other.m_properties);
}

void InvalidationSet::PropertySet::move_from(PropertySet&& other)
{
    m_storage_type = other.m_storage_type;
    if (m_storage_type == StorageType::Empty) {
        new (&m_empty) Empty();
        return;
    }
    if (m_storage_type == StorageType::Single) {
        new (&m_property) Property(move(other.m_property));
        return;
    }
    new (&m_properties) HashTable<Property>(move(other.m_properties));
}

bool InvalidationSet::PropertySet::is_empty() const
{
    return m_storage_type == StorageType::Empty;
}

size_t InvalidationSet::PropertySet::size() const
{
    if (m_storage_type == StorageType::Empty)
        return 0;
    if (m_storage_type == StorageType::Single)
        return 1;
    return m_properties.size();
}

bool InvalidationSet::PropertySet::contains(Property const& property) const
{
    if (m_storage_type == StorageType::Empty)
        return false;
    if (m_storage_type == StorageType::Single)
        return m_property == property;
    return m_properties.contains(property);
}

bool InvalidationSet::PropertySet::set(Property property)
{
    if (m_storage_type == StorageType::Empty) {
        new (&m_property) Property(move(property));
        m_storage_type = StorageType::Single;
        return true;
    }

    if (m_storage_type == StorageType::Single) {
        if (m_property == property)
            return false;

        HashTable<Property> properties;
        properties.set(m_property, AK::HashSetExistingEntryBehavior::Keep);
        properties.set(move(property), AK::HashSetExistingEntryBehavior::Keep);
        m_property.~Property();
        new (&m_properties) HashTable<Property>(move(properties));
        m_storage_type = StorageType::HashTable;
        return true;
    }

    return m_properties.set(move(property), AK::HashSetExistingEntryBehavior::Keep) == AK::HashSetResult::InsertedNewEntry;
}

bool InvalidationSet::PropertySet::include_all_from(PropertySet const& other)
{
    if (other.m_storage_type == StorageType::Empty)
        return false;
    if (other.m_storage_type == StorageType::Single)
        return set(other.m_property);

    bool changed = false;
    for (auto const& property : other.m_properties)
        changed |= set(property);
    return changed;
}

bool InvalidationSet::PropertySet::operator==(PropertySet const& other) const
{
    if (size() != other.size())
        return false;
    if (m_storage_type == StorageType::Empty)
        return true;
    if (m_storage_type == StorageType::Single)
        return other.contains(m_property);

    for (auto const& property : m_properties) {
        if (!other.contains(property))
            return false;
    }
    return true;
}

void InvalidationSet::PropertySet::accumulate_hash(u32& property_hash_sum, u32& property_hash_xor) const
{
    auto add_property_hash = [&](auto const& property) {
        auto property_hash = AK::Traits<Property>::hash(property);
        property_hash_sum += property_hash;
        property_hash_xor ^= pair_int_hash(property_hash, 0x9e3779b9);
    };

    if (m_storage_type == StorageType::Empty)
        return;
    if (m_storage_type == StorageType::Single) {
        add_property_hash(m_property);
        return;
    }

    for (auto const& property : m_properties)
        add_property_hash(property);
}

IterationDecision InvalidationSet::PropertySet::for_each(Function<IterationDecision(Property const&)> const& callback) const
{
    if (m_storage_type == StorageType::Empty)
        return IterationDecision::Continue;
    if (m_storage_type == StorageType::Single)
        return callback(m_property);

    for (auto const& property : m_properties) {
        if (callback(property) == IterationDecision::Break)
            return IterationDecision::Break;
    }
    return IterationDecision::Continue;
}

bool InvalidationSet::operator==(InvalidationSet const& other) const
{
    if (hash() != other.hash())
        return false;
    if (m_needs_invalidate_self != other.m_needs_invalidate_self)
        return false;
    if (m_needs_invalidate_whole_subtree != other.m_needs_invalidate_whole_subtree)
        return false;
    return m_properties == other.m_properties;
}

void InvalidationSet::include_all_from(InvalidationSet const& other)
{
    bool changed = false;
    if (other.m_needs_invalidate_self && !m_needs_invalidate_self) {
        m_needs_invalidate_self = true;
        changed = true;
    }
    if (other.m_needs_invalidate_whole_subtree && !m_needs_invalidate_whole_subtree) {
        m_needs_invalidate_whole_subtree = true;
        changed = true;
    }
    changed |= m_properties.include_all_from(other.m_properties);
    if (changed)
        m_hash = {};
}

bool InvalidationSet::is_empty() const
{
    return !m_needs_invalidate_self && !m_needs_invalidate_whole_subtree && m_properties.is_empty();
}

void InvalidationSet::add_property(Property property)
{
    if (m_properties.set(move(property)))
        m_hash = {};
}

void InvalidationSet::for_each_property(Function<IterationDecision(Property const&)> const& callback) const
{
    if (m_needs_invalidate_self) {
        if (callback({ Property::Type::InvalidateSelf }) == IterationDecision::Break)
            return;
    }
    if (m_needs_invalidate_whole_subtree) {
        if (callback({ Property::Type::InvalidateWholeSubtree }) == IterationDecision::Break)
            return;
    }
    m_properties.for_each(callback);
}

u32 InvalidationSet::hash() const
{
    if (m_hash.has_value())
        return *m_hash;

    u32 property_hash_sum = 0;
    u32 property_hash_xor = 0;
    m_properties.accumulate_hash(property_hash_sum, property_hash_xor);

    auto hash = pair_int_hash(m_needs_invalidate_self, m_needs_invalidate_whole_subtree);
    hash = pair_int_hash(hash, m_properties.size());
    hash = pair_int_hash(hash, property_hash_sum);
    hash = pair_int_hash(hash, property_hash_xor);

    m_hash = hash;
    return hash;
}

}

namespace AK {

unsigned Traits<Web::CSS::InvalidationSet::Property>::hash(Web::CSS::InvalidationSet::Property const& invalidation_set_property)
{
    auto value_hash = invalidation_set_property.value.visit(
        [](FlyString const& value) -> int { return value.hash(); },
        [](Web::CSS::PseudoClass const& value) -> int { return to_underlying(value); },
        [](Empty) -> int { return 0; });
    return pair_int_hash(to_underlying(invalidation_set_property.type), value_hash);
}

ErrorOr<void> Formatter<Web::CSS::InvalidationSet::Property>::format(FormatBuilder& builder, Web::CSS::InvalidationSet::Property const& invalidation_set_property)
{
    switch (invalidation_set_property.type) {
    case Web::CSS::InvalidationSet::Property::Type::InvalidateSelf: {
        TRY(builder.put_string("$"sv));
        return {};
    }
    case Web::CSS::InvalidationSet::Property::Type::Class: {
        TRY(builder.put_string("."sv));
        TRY(builder.put_string(invalidation_set_property.name()));
        return {};
    }
    case Web::CSS::InvalidationSet::Property::Type::Id: {
        TRY(builder.put_string("#"sv));
        TRY(builder.put_string(invalidation_set_property.name()));
        return {};
    }
    case Web::CSS::InvalidationSet::Property::Type::TagName: {
        TRY(builder.put_string(invalidation_set_property.name()));
        return {};
    }
    case Web::CSS::InvalidationSet::Property::Type::Attribute: {
        TRY(builder.put_string("["sv));
        TRY(builder.put_string(invalidation_set_property.name()));
        TRY(builder.put_string("]"sv));
        return {};
    }
    case Web::CSS::InvalidationSet::Property::Type::PseudoClass: {
        TRY(builder.put_string(":"sv));
        TRY(builder.put_string(pseudo_class_name(invalidation_set_property.value.get<Web::CSS::PseudoClass>())));
        return {};
    }
    case Web::CSS::InvalidationSet::Property::Type::InvalidateWholeSubtree: {
        TRY(builder.put_string("*"sv));
        return {};
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<void> Formatter<Web::CSS::InvalidationSet>::format(FormatBuilder& builder, Web::CSS::InvalidationSet const& invalidation_set)
{
    bool first = true;
    invalidation_set.for_each_property([&](auto const& property) {
        if (!first)
            builder.builder().append(", "sv);
        builder.builder().appendff("{}", property);
        return IterationDecision::Continue;
    });
    return {};
}

}
