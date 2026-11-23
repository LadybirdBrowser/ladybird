/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <UI/Qt/SelectDropdown.h>
#include <UI/Qt/StringUtils.h>

#include <QAction>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QListWidget>
#include <QScreen>
#include <QScrollBar>
#include <QVariant>
#include <QWidgetAction>

namespace Ladybird {

class SelectOptionList final : public QListWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return) {
            emit itemActivated(currentItem());
            event->accept();
            return;
        }
        QListWidget::keyPressEvent(event);
    }
};

SelectDropdown::SelectDropdown(QWidget* parent)
    : QMenu("Select Dropdown", parent)
{
    // NB: The options are sized to match the 13.333px font a select uses by default. A point size would grow with the
    //     screen's DPI on some platforms, so the size is in pixels.
    setObjectName("LadybirdSelectDropdown");
    auto menu_font = font();
    menu_font.setPixelSize(13);
    setFont(menu_font);
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

    constexpr int maximum_visible_rows = 20;
    Optional<u32> chosen_item_id;
    QObject selection_context;
    auto const menu_actions = actions();
    if (menu_actions.size() > maximum_visible_rows) {
        auto* list = new SelectOptionList;
        list->setFont(font());
        list->setFrameShape(QFrame::NoFrame);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        int row_height = 0;
        for (auto* action : menu_actions) {
            auto* item = new QListWidgetItem(action->text(), list);
            item->setData(Qt::UserRole, action->data());
            auto size = actionGeometry(action).size();
            item->setSizeHint(size);
            row_height = qMax(row_height, size.height());
            if (action->isChecked()) {
                item->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton, nullptr, list));
                list->setCurrentItem(item);
            }
            if (action->isSeparator()) {
                auto* line = new QFrame;
                line->setFrameShape(QFrame::HLine);
                list->setItemWidget(item, line);
            }
            if (!action->isEnabled() || action->isSeparator())
                item->setFlags(Qt::NoItemFlags);
        }
        clear();
        auto* screen = QGuiApplication::screenAt(global_position);
        if (!screen)
            screen = this->screen();
        auto available = screen->availableGeometry();
        int const width = qMax(minimum_width, list->sizeHintForColumn(0) + list->verticalScrollBar()->sizeHint().width());
        auto panel_width = style()->pixelMetric(QStyle::PM_MenuPanelWidth, nullptr, this);
        list->setFixedSize(QSize(width, maximum_visible_rows * row_height).boundedTo(available.size() - QSize(2 * panel_width, 2 * panel_width)));
        auto* list_action = new QWidgetAction(this);
        list_action->setDefaultWidget(list);
        addAction(list_action);
        auto choose = [&](QListWidgetItem* item) {
            if (item && item->flags().testFlag(Qt::ItemIsEnabled) && item->data(Qt::UserRole).isValid()) {
                chosen_item_id = item->data(Qt::UserRole).toUInt();
                close();
            }
        };
        connect(list, &QListWidget::itemClicked, &selection_context, choose);
        connect(list, &QListWidget::itemActivated, &selection_context, choose);
        list->setFocus();
    }

    // A highlighted action alone does not indicate a choice: Escape and an outside click also dismiss the menu.
    if (auto* action = exec(global_position); action && action->data().isValid())
        chosen_item_id = action->data().toUInt();

    if (exchange(m_suppress_close_report, false))
        return;
    if (on_closed)
        on_closed(chosen_item_id);
}

void SelectDropdown::close_without_reporting()
{
    if (!isVisible())
        return;
    m_suppress_close_report = true;
    close();
}

}
