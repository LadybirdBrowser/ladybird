/*
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>
#include <LibWebView/Forward.h>
#include <UI/Qt/WebContentView.h>

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

class QVariantAnimation;

namespace Ladybird {

class Tab;
class WebContentView;

class FindInPageWidget final : public QWidget {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    FindInPageWidget(Tab* tab, WebContentView* content_view);

    void update_result_label(size_t current_match_index, Optional<size_t> const& total_match_count);

    void find_previous();
    void find_next();
    void reposition();
    void focus_search_field();

    virtual ~FindInPageWidget() override;

public slots:

private:
    virtual bool event(QEvent*) override;
    virtual void keyPressEvent(QKeyEvent*) override;
    virtual void focusInEvent(QFocusEvent*) override;
    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;
    virtual void paintEvent(QPaintEvent*) override;

    void close_bar();
    void find_text_changed();
    void update_chrome_style();
    void update_icons();
    void set_has_no_matches(bool);

    Tab* m_tab { nullptr };
    WebContentView* m_content_view { nullptr };

    QLineEdit* m_find_text { nullptr };
    QPushButton* m_previous_button { nullptr };
    QPushButton* m_next_button { nullptr };
    QPushButton* m_exit_button { nullptr };
    QPushButton* m_match_case { nullptr };
    QWidget* m_field { nullptr };
    QLabel* m_search_icon { nullptr };
    QLabel* m_result_label { nullptr };
    bool m_is_updating_chrome_style { false };
    bool m_has_no_matches { false };
    u64 m_selected_text_request_id { 0 };
    // The bar slides down this far into place as it opens.
    qreal m_opening_offset { 0 };
    QVariantAnimation* m_opening_animation { nullptr };
    QWidget* m_shadow { nullptr };
};

}
