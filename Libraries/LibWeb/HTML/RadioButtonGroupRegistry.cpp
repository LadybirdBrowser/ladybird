/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

    if (group.members.is_empty())
        m_groups.remove(it);
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
    auto const& group = it->value;
    return group.required_count > 0 && !group.checked_button;
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

}
