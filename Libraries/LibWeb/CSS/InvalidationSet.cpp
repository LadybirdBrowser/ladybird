/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/InvalidationSet.h>

namespace Web::CSS {

void InvalidationSet::include_all_from(InvalidationSet const& other)
{
    m_needs_invalidate_self |= other.m_needs_invalidate_self;
    m_needs_invalidate_whole_subtree |= other.m_needs_invalidate_whole_subtree;
    for (auto const& property : other.m_properties)
        m_properties.set(property);
}

bool InvalidationSet::is_empty() const
{
    return !m_needs_invalidate_self && !m_needs_invalidate_whole_subtree && m_properties.is_empty();
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
    for (auto const& property : m_properties) {
        if (callback(property) == IterationDecision::Break)
            return;
    }
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
