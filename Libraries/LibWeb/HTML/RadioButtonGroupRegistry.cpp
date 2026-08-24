/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/FormControlInvalidator.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/RadioButtonGroupRegistry.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(RadioButtonGroupRegistry);

void RadioButtonGroupRegistry::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& [name, group] : m_groups) {
        for (auto const& member : group.members)
            visitor.visit(member);
        visitor.visit(group.checked_button);
    }
}

void RadioButtonGroupRegistry::add_button(Utf16FlyString const& group_name, HTMLInputElement& button)
{
    VERIFY(!group_name.is_empty());
    auto& group = m_groups.ensure(group_name);
    auto result = group.members.set(button);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);

    if (button.has_attribute(AttributeNames::required))
        ++group.required_count;
    if (button.checked())
        set_checked_button(group, button);

    update_group_suffering_from_being_missing(group);
}

void RadioButtonGroupRegistry::remove_button(Utf16FlyString const& group_name, HTMLInputElement& button)
{
    auto it = m_groups.find(group_name);
    VERIFY(it != m_groups.end());
    auto& group = it->value;
    VERIFY(group.members.remove(button));

    if (button.has_attribute(AttributeNames::required)) {
        VERIFY(group.required_count > 0);
        --group.required_count;
    }
    if (group.checked_button.ptr() == &button)
        group.checked_button = nullptr;

    if (group.members.is_empty()) {
        m_groups.remove(it);
        return;
    }
    update_group_suffering_from_being_missing(group);
}

void RadioButtonGroupRegistry::checked_state_changed(Utf16FlyString const& group_name, HTMLInputElement& button)
{
    auto it = m_groups.find(group_name);
    VERIFY(it != m_groups.end());
    auto& group = it->value;
    VERIFY(group.members.contains(button));

    if (button.checked())
        set_checked_button(group, button);
    else if (group.checked_button.ptr() == &button)
        group.checked_button = nullptr;

    update_group_suffering_from_being_missing(group);
}

void RadioButtonGroupRegistry::required_state_changed(Utf16FlyString const& group_name, HTMLInputElement& button)
{
    auto it = m_groups.find(group_name);
    VERIFY(it != m_groups.end());
    auto& group = it->value;
    VERIFY(group.members.contains(button));

    if (button.has_attribute(AttributeNames::required)) {
        ++group.required_count;
    } else {
        VERIFY(group.required_count > 0);
        --group.required_count;
    }

    update_group_suffering_from_being_missing(group);
}

GC::Ptr<HTMLInputElement> RadioButtonGroupRegistry::checked_button(Utf16FlyString const& group_name) const
{
    auto it = m_groups.find(group_name);
    VERIFY(it != m_groups.end());
    return it->value.checked_button;
}

bool RadioButtonGroupRegistry::group_is_suffering_from_being_missing(Utf16FlyString const& group_name) const
{
    auto it = m_groups.find(group_name);
    VERIFY(it != m_groups.end());
    return it->value.suffering_from_being_missing;
}

void RadioButtonGroupRegistry::set_checked_button(RadioButtonGroup& group, HTMLInputElement& button)
{
    auto old_checked_button = group.checked_button;
    if (old_checked_button.ptr() == &button)
        return;
    group.checked_button = &button;

    if (old_checked_button)
        old_checked_button->set_checked(false);
}

void RadioButtonGroupRegistry::update_group_suffering_from_being_missing(RadioButtonGroup& group)
{
    auto suffering_from_being_missing = group.required_count > 0 && !group.checked_button;
    if (suffering_from_being_missing == group.suffering_from_being_missing)
        return;
    group.suffering_from_being_missing = suffering_from_being_missing;

    // Every member answers value-missing validity queries with the state of its group, so a change to the group's
    // state changes which validity pseudo-classes match on all of them.
    for (auto const& member : group.members)
        CSS::Invalidation::invalidate_style_after_validity_change(member);
}

}
