/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <UI/Qt/SelectDropdown.h>
#include <UI/Qt/StringUtils.h>

#include <QAction>
#include <QVariant>

namespace Ladybird {

SelectDropdown::SelectDropdown(QWidget* parent)
    : QMenu("Select Dropdown", parent)
{
}

void SelectDropdown::open(QPoint const& global_position, int minimum_width, Vector<Web::HTML::SelectItem> const& items)
{
    m_suppress_close_report = false;
    clear();
    setMinimumWidth(minimum_width);

    auto add_menu_item = [this](Web::HTML::SelectItemOption const& item_option, bool in_option_group) {
        auto label = in_option_group ? qformatted("    {}", item_option.label) : qstring_from_utf16_string(item_option.label);

        QAction* action = new QAction(label, this);
        action->setCheckable(true);
        action->setChecked(item_option.selected);
        action->setDisabled(item_option.disabled);
        action->setData(QVariant(static_cast<uint>(item_option.id)));
        addAction(action);
    };

    for (auto const& item : items) {
        if (item.has<Web::HTML::SelectItemOptionGroup>()) {
            auto const& item_option_group = item.get<Web::HTML::SelectItemOptionGroup>();
            QAction* subtitle = new QAction(qstring_from_utf16_string(item_option_group.label), this);
            subtitle->setDisabled(true);
            addAction(subtitle);

            for (auto const& item_option : item_option_group.items)
                add_menu_item(item_option, true);
        }

        if (item.has<Web::HTML::SelectItemOption>())
            add_menu_item(item.get<Web::HTML::SelectItemOption>(), false);

        if (item.has<Web::HTML::SelectItemSeparator>())
            addSeparator();
    }

    // NB: exec() returns the action the user chose, or null when the menu went away without a choice — and that covers
    //     every way a QMenu can close: Escape, a click elsewhere, the window deactivating. The aboutToHide signal can't
    //     tell those apart from a choice; Qt emits it with the highlighted action still current, before triggered()
    //     would fire, so a cancel with an item highlighted looks just like a choice there.
    auto* chosen_action = exec(global_position);

    if (exchange(m_suppress_close_report, false))
        return;
    if (!on_closed)
        return;

    if (chosen_action)
        on_closed(chosen_action->data().value<uint>());
    else
        on_closed({});
}

void SelectDropdown::close_without_reporting()
{
    if (!isVisible())
        return;
    m_suppress_close_report = true;
    close();
}

}
