/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Menu.h>

namespace WebView {

NonnullRefPtr<Action> Action::create(Variant<StringView, String> text, ActionID id, Function<void()> action)
{
    return adopt_ref(*new Action { move(text), id, move(action) });
}

NonnullRefPtr<Action> Action::create_checkable(Variant<StringView, String> text, ActionID id, Function<void()> action)
{
    auto checkable = create(move(text), id, move(action));
    checkable->m_checked = false;
    return checkable;
}

void Action::set_text(Variant<StringView, String> text)
{
    if (text.visit([&](auto const& text) { return text == this->text(); }))
        return;
    m_text = move(text);

    for (auto& observer : m_observers)
        observer->on_text_changed(*this);
}

void Action::set_tooltip(StringView tooltip)
{
    if (m_tooltip == tooltip)
        return;
    m_tooltip = tooltip;

    for (auto& observer : m_observers)
        observer->on_tooltip_changed(*this);
}

void Action::set_enabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;

    for (auto& observer : m_observers)
        observer->on_enabled_state_changed(*this);
}

void Action::set_visible(bool visible)
{
    if (m_visible == visible)
        return;
    m_visible = visible;

    for (auto& observer : m_observers)
        observer->on_visible_state_changed(*this);
}

void Action::set_checked(bool checked)
{
    set_checked_internal(checked);

    if (auto group = m_group.strong_ref()) {
        group->for_each_action([&](Action& action) {
            if (action.is_checkable() && &action != this)
                action.set_checked_internal(false);
        });
    }
}

void Action::set_checked_internal(bool checked)
{
    VERIFY(is_checkable());

    if (m_checked == checked)
        return;
    m_checked = checked;

    for (auto& observer : m_observers)
        observer->on_checked_state_changed(*this);
}

void Action::add_observer(NonnullOwnPtr<Observer> observer)
{
    observer->on_text_changed(*this);
    if (m_tooltip.has_value())
        observer->on_tooltip_changed(*this);
    observer->on_enabled_state_changed(*this);
    observer->on_visible_state_changed(*this);
    if (is_checkable())
        observer->on_checked_state_changed(*this);

    m_observers.append(move(observer));
}

void Action::remove_observer(Observer const& observer)
{
    m_observers.remove_first_matching([&](auto const& candidate) {
        return candidate.ptr() == &observer;
    });
}

NonnullRefPtr<Menu> Menu::create(StringView name)
{
    return adopt_ref(*new Menu { name });
}

NonnullRefPtr<Menu> Menu::create_group(StringView name)
{
    auto menu = create(name);
    menu->m_is_group = true;
    return menu;
}

void Menu::add_action(NonnullRefPtr<Action> action)
{
    if (m_is_group)
        action->set_group({}, *this);
    m_items.append(move(action));
}

}
