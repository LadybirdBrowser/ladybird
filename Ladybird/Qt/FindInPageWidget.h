/*
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "WebContentView.h"
#include <LibWebView/Forward.h>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

namespace Ladybird {

class WebContentView;

class FindInPageWidget final : public QWidget {
    Q_OBJECT

public:
    FindInPageWidget(Tab* tab, WebContentView* content_view);

    virtual ~FindInPageWidget() override;

public slots:

private:
    virtual void keyPressEvent(QKeyEvent*) override;
    virtual void focusInEvent(QFocusEvent*) override;
    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;

    void find_text_changed();

    Tab* m_tab { nullptr };
    WebContentView* m_content_view { nullptr };

    QLineEdit* m_find_text { nullptr };
    QPushButton* m_previous_button { nullptr };
    QPushButton* m_next_button { nullptr };
    QPushButton* m_exit_button { nullptr };
    QCheckBox* m_match_case { nullptr };
};

}
