/*
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FindInPageWidget.h"
#include "Icon.h"
#include "StringUtils.h"
#include <Ladybird/Qt/Tab.h>
#include <QKeyEvent>

namespace Ladybird {

FindInPageWidget::FindInPageWidget(Tab* tab, WebContentView* content_view)
    : QWidget(static_cast<QWidget*>(tab), Qt::Widget)
    , m_tab(tab)
    , m_content_view(content_view)
{
    setFocusPolicy(Qt::FocusPolicy::StrongFocus);

    auto* layout = new QHBoxLayout(this);
    setLayout(layout);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setAlignment(Qt::AlignmentFlag::AlignLeft);

    m_find_text = new QLineEdit(this);
    m_find_text->setFocusPolicy(Qt::FocusPolicy::StrongFocus);
    m_find_text->setSizePolicy(QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Fixed);
    m_find_text->setPlaceholderText("Search");
    m_find_text->setMinimumWidth(50);
    m_find_text->setMaximumWidth(250);
    connect(m_find_text, &QLineEdit::textChanged, this, &FindInPageWidget::find_text_changed);

    m_previous_button = new QPushButton(this);
    m_previous_button->setFixedWidth(30);
    m_previous_button->setIcon(create_tvg_icon_with_theme_colors("up", palette()));
    m_previous_button->setToolTip("Find Previous Match");
    m_previous_button->setFlat(true);
    connect(m_previous_button, &QPushButton::clicked, this, [this] {
        m_content_view->find_in_page_previous_match();
    });

    m_next_button = new QPushButton(this);
    m_next_button->setFixedWidth(30);
    m_next_button->setIcon(create_tvg_icon_with_theme_colors("down", palette()));
    m_next_button->setToolTip("Find Next Match");
    m_next_button->setFlat(true);
    connect(m_next_button, &QPushButton::clicked, this, [this] {
        m_content_view->find_in_page_next_match();
    });

    m_exit_button = new QPushButton(this);
    m_exit_button->setFixedWidth(30);
    m_exit_button->setIcon(create_tvg_icon_with_theme_colors("close", palette()));
    m_exit_button->setToolTip("Close Search Bar");
    m_exit_button->setFlat(true);
    connect(m_exit_button, &QPushButton::clicked, this, [this] {
        setVisible(false);
    });

    m_match_case = new QCheckBox(this);
    m_match_case->setText("Match &Case");
    m_match_case->setChecked(false);
#if (QT_VERSION > QT_VERSION_CHECK(6, 7, 0))
    connect(m_match_case, &QCheckBox::checkStateChanged, this, [this] {
#else
    connect(m_match_case, &QCheckBox::stateChanged, this, [this] {
#endif
        find_text_changed();
    });

    m_result_label = new QLabel(this);
    m_result_label->setVisible(false);
    m_result_label->setStyleSheet("font-weight: bold;");

    layout->addWidget(m_find_text, 1);
    layout->addWidget(m_previous_button);
    layout->addWidget(m_next_button);
    layout->addWidget(m_match_case);
    layout->addWidget(m_result_label);
    layout->addStretch(1);
    layout->addWidget(m_exit_button);
}

FindInPageWidget::~FindInPageWidget() = default;

void FindInPageWidget::find_text_changed()
{
    auto query = ak_string_from_qstring(m_find_text->text());
    auto case_sensitive = m_match_case->isChecked() ? CaseSensitivity::CaseSensitive : CaseSensitivity::CaseInsensitive;
    m_content_view->find_in_page(query, case_sensitive);
}

void FindInPageWidget::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        setVisible(false);
        break;
    case Qt::Key_Return:
        if (event->modifiers().testFlag(Qt::ShiftModifier))
            m_previous_button->click();
        else
            m_next_button->click();
        break;
    default:
        event->ignore();
        break;
    }
}

void FindInPageWidget::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    m_find_text->setFocus();
    auto selected_text = m_content_view->selected_text();
    if (!selected_text.is_empty())
        m_find_text->setText(qstring_from_ak_string(selected_text));
    m_find_text->selectAll();
}

void FindInPageWidget::showEvent(QShowEvent*)
{
    if (m_tab && m_tab->isVisible())
        m_tab->update_hover_label();
}

void FindInPageWidget::hideEvent(QHideEvent*)
{
    if (m_tab && m_tab->isVisible())
        m_tab->update_hover_label();
}

void FindInPageWidget::update_result_label(size_t current_match_index, Optional<size_t> const& total_match_count)
{
    if (total_match_count.has_value()) {
        auto label_text = "Phrase not found"_string;
        if (total_match_count.value() > 0)
            label_text = MUST(String::formatted("{} of {} matches", current_match_index + 1, total_match_count.value()));

        m_result_label->setText(qstring_from_ak_string(label_text));
        m_result_label->setVisible(true);
    } else {
        m_result_label->setVisible(false);
    }
}

void FindInPageWidget::find_previous()
{
    m_previous_button->click();
}

void FindInPageWidget::find_next()
{
    m_next_button->click();
}

}
